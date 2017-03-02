#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <vector>
#include <set>
#include <chrono>
#include "common/interval_set.h"
#include "common/utils.h"
#include "rpc/message.pb.h"
#include "snapshot_proxy.h"

using huawei::proto::SnapshotMessage;
using huawei::proto::SnapScene;
using huawei::proto::VolumeStatus;
using huawei::proto::RepStatus;
using huawei::proto::RepRole;

using huawei::proto::inner::CreateReq;
using huawei::proto::inner::CreateAck;
using huawei::proto::inner::ListReq;
using huawei::proto::inner::ListAck;
using huawei::proto::inner::QueryReq;
using huawei::proto::inner::QueryAck;
using huawei::proto::inner::DeleteReq;
using huawei::proto::inner::DeleteAck;
using huawei::proto::inner::CowReq;
using huawei::proto::inner::CowAck;
using huawei::proto::inner::UpdateReq;
using huawei::proto::inner::UpdateAck;
using huawei::proto::inner::CowUpdateReq;
using huawei::proto::inner::CowUpdateAck;
using huawei::proto::inner::RollbackReq;
using huawei::proto::inner::RollbackAck;
using huawei::proto::inner::DiffReq;
using huawei::proto::inner::DiffAck;
using huawei::proto::inner::ReadReq;
using huawei::proto::inner::ReadAck;
using huawei::proto::inner::SyncReq;
using huawei::proto::inner::SyncAck;
using huawei::proto::inner::ReadBlock;
using huawei::proto::inner::RollBlock;
using huawei::proto::inner::UpdateEvent;

#define ALIGN_UP(v,align) (((v)+(align)-1) & ~((align)-1))

bool SnapshotProxy::init()
{        
    /*snapshot inner rpc client stub*/
    m_rpc_stub = SnapshotInnerControl::NewStub(grpc::CreateChannel(m_conf.sg_server_addr(), 
                grpc::InsecureChannelCredentials()));
   
    /*open block device*/
    m_block_fd = open(m_vol_attr.blk_device().c_str(), O_RDWR | O_DIRECT | O_SYNC);
    if(m_block_fd == -1){
        LOG_ERROR << "open block device:" << m_vol_attr.blk_device().c_str() 
                  << " errno:" << errno; 
        return false;
    }
    
    LOG_INFO << "open block device:" << m_vol_attr.blk_device().c_str()
             << " m_block_fd:" << m_block_fd;

    /*snapshot block store*/
    m_block_store = new CephBlockStore();
    
    m_sync_table.clear();
    m_active_snapshot.clear();
    m_exist_snapshot = false;
    
    /*sync with dr server*/
    sync_state();

    return true;
}

bool SnapshotProxy::fini()
{
    m_sync_table.clear();
    m_active_snapshot.clear();
    m_exist_snapshot = false;
 
    if(m_block_store){
        delete m_block_store;
    }

    if(m_block_fd != -1){
        close(m_block_fd); 
    }

    return true;
}

StatusCode SnapshotProxy::sync_state()
{
    LOG_INFO << "sync_state";

    ClientContext context;
    SyncReq ireq;
    ireq.set_vol_name(m_vol_attr.vol_name());
    SyncAck iack;
    Status st = m_rpc_stub->Sync(&context, ireq, &iack);
    if(!st.ok()) {
        LOG_INFO << "sync_state failed err:" << iack.header().status();
        return iack.header().status();
    }

    m_active_snapshot = iack.latest_snap_name();
    if(!m_active_snapshot.empty()){
        m_exist_snapshot = true; 
    }

    LOG_INFO << "sync_state" << " ret:" << iack.header().status();
    return StatusCode::sOk;
}

void SnapshotProxy::cmd_persist_wait()
{
    unique_lock<std::mutex> ulock(m_cmd_persist_lock);
    m_cmd_persist_cond.wait(ulock);
}

void SnapshotProxy::cmd_persist_notify(const JournalMarker& mark)
{
    unique_lock<std::mutex> ulock(m_cmd_persist_lock);
    m_cmd_persist_mark = mark;
    m_cmd_persist_cond.notify_all();
}

void SnapshotProxy::add_sync(const string& actor, const string& action)
{
    m_sync_table.insert({actor, action});
}

void SnapshotProxy::del_sync(const string& actor)
{
    m_sync_table.erase(actor);
}

bool SnapshotProxy::check_sync_on(const string& actor)
{
    auto it = m_sync_table.find(actor);
    if(it == m_sync_table.end())
        return false;
    return true;
}

StatusCode SnapshotProxy::create_snapshot(const CreateSnapshotReq* req, 
                                CreateSnapshotAck* ack){
    JournalMarker m;
    return create_snapshot(req,ack,m);
}

StatusCode SnapshotProxy::create_snapshot(const CreateSnapshotReq* req, 
                                CreateSnapshotAck* ack,JournalMarker& marker)
{
    /*get from exterior rpc*/
    string vname = req->vol_name();
    string sname = req->snap_name();
    SnapType snap_type = req->header().snap_type();

    LOG_INFO << "create_snapshot vname:" << vname << " sname:" << sname;
    if(!m_vol_attr.is_snapshot_allowable(snap_type)){
        LOG_ERROR << "create_snapshot vname:" << vname << " sname:" << sname << " denied";
        return StatusCode::sSnapCreateDenied;
    }
    
    StatusCode ret_code = StatusCode::sOk;
    /*sync begin*/
    add_sync(sname, "snapshot on creating");

    if(m_vol_attr.is_append_entry_need(snap_type)){
        /*spawn journal entry*/
        shared_ptr<JournalEntry> entry = spawn_journal_entry(req->header(),sname,
                                                             SNAPSHOT_CREATE); 
        /*push journal entry to entry queue*/
        m_entry_queue.push(entry);
        /*todo: wait journal writer persist journal entry ok and ack*/
        cmd_persist_wait();

        marker.CopyFrom(m_cmd_persist_mark);
        LOG_INFO << "create_snapshot vname:" << vname << " sname:" << sname
            << " journal:" << m_cmd_persist_mark.cur_journal() 
            << " pos:" << m_cmd_persist_mark.pos();
    }

    /*rpc with dr_server */
    ret_code = do_create(req->header(), sname);

    LOG_INFO << "create_snapshot vname:" << vname << " sname:" << sname 
             << (!ret_code ? "ok" : "failed, rpc error");

    /*sync end*/
    del_sync(sname);
    ack->mutable_header()->set_status(ret_code);
    return ret_code;
}

StatusCode SnapshotProxy::list_snapshot(const ListSnapshotReq* req, 
                                        ListSnapshotAck* ack)
{
    string vname = req->vol_name();
    LOG_INFO << "list_snapshot vname:" << vname;

    ClientContext context;
    ListReq ireq;
    ireq.set_vol_name(vname);
    ListAck iack;
    Status st = m_rpc_stub->List(&context, ireq, &iack);
    if(!st.ok()){
        return iack.header().status(); 
    }

    int snap_num = iack.snap_name_size();
    for(int i = 0; i < snap_num; i++){
       ack->add_snap_name(iack.snap_name(i)); 
    }
    LOG_INFO << "list_snapshot vname:" << vname
             << " snap_size:" << iack.snap_name_size() << " ok";
    return StatusCode::sOk; 
}

StatusCode SnapshotProxy::query_snapshot(const QuerySnapshotReq* req, 
                                         QuerySnapshotAck* ack)
{
    string vname = req->vol_name();
    string sname = req->snap_name();
    LOG_INFO << "query_snapshot vname:" << vname << " sname:" << sname;

    ClientContext context;
    QueryReq ireq;
    ireq.set_vol_name(vname);
    ireq.set_snap_name(sname);
    QueryAck iack;
    Status st = m_rpc_stub->Query(&context, ireq, &iack);
    if(!st.ok()){
        return iack.header().status();
    }
    ack->set_snap_status(iack.snap_status());

    LOG_INFO << "query_snapshot vname:" << vname << " sname:" << sname << " ok";
    return StatusCode::sOk; 
}

StatusCode SnapshotProxy::delete_snapshot(const DeleteSnapshotReq* req, 
                                          DeleteSnapshotAck* ack)
{
    string vname = req->vol_name();
    string sname = req->snap_name();
    SnapType snap_type = req->header().snap_type();

    LOG_INFO << "delete_snapshot vname:" << vname << " sname:" << sname;

    StatusCode ret_code = StatusCode::sOk;
    /*sync begin*/
    add_sync(sname, "snapshot on deleting");

    if(m_vol_attr.is_append_entry_need(snap_type)){
        /*spawn journal entry*/
        shared_ptr<JournalEntry> entry = spawn_journal_entry(req->header(), sname, 
                                                             SNAPSHOT_DELETE);
        /*push journal entry to write queue*/
        m_entry_queue.push(entry);
        cmd_persist_wait();
    }

   /*rpc with dr_server */
    ret_code = do_delete(req->header(), sname);

    LOG_INFO << "delete vname:" << vname << " sname:" << sname 
             << (!ret_code ? "ok" : "failed, rpc error");

    /*sync end*/
    del_sync(sname);
    ack->mutable_header()->set_status(ret_code);
    return ret_code;
}

StatusCode SnapshotProxy::rollback_snapshot(const RollbackSnapshotReq* req, 
                                            RollbackSnapshotAck* ack)
{
    string vname = req->vol_name();
    string sname = req->snap_name();
    SnapType snap_type = req->header().snap_type();

    LOG_INFO << "rollback_snapshot vname:" << vname << " sname:" << sname;
    
    StatusCode ret_code = StatusCode::sOk;
    add_sync(sname, "snapshot on rollbacking") ;
 
    if(m_vol_attr.is_append_entry_need(snap_type)){
        /*spawn journal entry*/
        shared_ptr<JournalEntry> entry = spawn_journal_entry(req->header(),sname, 
                                                             SNAPSHOT_ROLLBACK);
        /*push journal entry to write queue*/
        m_entry_queue.push(entry);
        /*wait journal writer persist journal entry ok and ack*/
        cmd_persist_wait();
    }

    ret_code = do_rollback(req->header(), sname);
    LOG_INFO << "rollback_snapshot vname:" << vname << " sname:" << sname 
             << (!ret_code ? "ok" : "failed, rpc error");
    del_sync(sname);
    return ret_code;
}

StatusCode SnapshotProxy::create_transaction(const SnapReqHead& shead, 
                                             const string& snap_name)
{
    LOG_INFO << "create transaction begin";
    StatusCode ret = transaction(shead, snap_name, UpdateEvent::CREATE_EVENT);
    if(ret){
        /*create snapshot failed, delete the snapshot*/
        ret = do_update(shead, snap_name, UpdateEvent::DELETE_EVENT);
        LOG_ERROR << "create transaction failed ret:" << ret;
        return ret;
    }
    
    LOG_INFO << "create transaction end";
    return ret;
}

StatusCode SnapshotProxy::delete_transaction(const SnapReqHead& shead, 
                                             const string& snap_name)
{
    LOG_INFO << "delete transaction begin";
    StatusCode ret = transaction(shead, snap_name, UpdateEvent::DELETE_EVENT);
    if(ret){
        LOG_ERROR << "delete transaction failed";
        return ret;
    }
    
    LOG_INFO << "delete transaction end";
    return ret;
}

StatusCode SnapshotProxy::rollback_transaction(const SnapReqHead& shead, 
                                               const string& snap_name)
{
    LOG_INFO << "rollback transaction begin";
    StatusCode ret = transaction(shead, snap_name, UpdateEvent::ROLLBACK_EVENT);
    if(ret){
        LOG_ERROR << "rollback transaction failed";
        return ret;
    }

    ClientContext context;
    RollbackReq ireq;
    ireq.mutable_header()->CopyFrom(shead);
    ireq.set_vol_name(m_vol_attr.vol_name());
    ireq.set_snap_name(snap_name);
    RollbackAck iack;
    Status st = m_rpc_stub->Rollback(&context, ireq, &iack);
    if(!st.ok()){
        return iack.header().status();
    }

    int roll_blk_num = iack.roll_blocks_size();
    for(int i = 0; i < roll_blk_num; i++){
        RollBlock roll_block = iack.roll_blocks(i);
        LOG_INFO << "do rollback blk_no:" << roll_block.blk_no() 
                 << " blk_object:" << roll_block.blk_object(); 

        /*read latest data from block device*/
        void* block_buf = nullptr;
        int ret = posix_memalign((void**)&block_buf, 512, COW_BLOCK_SIZE);
        assert(ret == 0 && block_buf != nullptr);
        off_t block_off  = roll_block.blk_no() * COW_BLOCK_SIZE;
        off_t block_size = COW_BLOCK_SIZE;
        size_t read_ret = raw_device_read((char*)block_buf, 
                                          COW_BLOCK_SIZE, 
                                          block_off); 
        assert(read_ret == COW_BLOCK_SIZE);
        
        /*do cow*/
        ret = do_cow(block_off, block_size, (char*)block_buf, true);
        assert(ret == StatusCode::sOk); 
        free(block_buf);

        /*rollback*/
        string roll_block_object = roll_block.blk_object();
        void* roll_buf = nullptr;
        ret = posix_memalign((void**)&roll_buf, 512, COW_BLOCK_SIZE);
        assert(ret == 0 && roll_buf != nullptr);
        ret = m_block_store->read(roll_block_object, (char*)roll_buf, 
                                  block_size, 0);
        assert(ret == block_size);
        size_t write_ret = raw_device_write((char*)roll_buf, 
                                             block_size, 
                                             block_off); 
        assert(read_ret == COW_BLOCK_SIZE);
    }
    
    /*dr server to delete rollback snapshot*/
    ret = do_update(shead, snap_name, UpdateEvent::ROLLBACK_EVENT);

    LOG_INFO << "rollback transaction end";
    return ret;
}

shared_ptr<JournalEntry> SnapshotProxy::spawn_journal_entry(
                            const SnapReqHead& shead,
                            const string& sname,
                            const journal_event_type_t& entry_type)
{
    /*spawn snapshot message*/
    shared_ptr<SnapshotMessage> message = make_shared<SnapshotMessage>();
    message->set_replication_uuid(shead.replication_uuid());
    message->set_checkpoint_uuid(shead.checkpoint_uuid());
    message->set_vol_name(m_vol_attr.vol_name()); 
    message->set_snap_scene(shead.scene());
    message->set_snap_type(shead.snap_type());
    message->set_snap_name(sname); 

    /*spawn journal entry*/
    shared_ptr<JournalEntry> entry = make_shared<JournalEntry>();
    entry->set_type(entry_type);
    entry->set_message(message);
    return entry; 
}

StatusCode SnapshotProxy::transaction(const SnapReqHead& shead, 
                                      const string&      sname,
                                      const UpdateEvent& sevent) 
{
    StatusCode ret = StatusCode::sOk;
    while(check_sync_on(sname)){
        usleep(200);
    }        

    /*trigger dr server update snapshot status*/
    ret = do_update(shead, sname, sevent);
    if(ret){
        return StatusCode::sSnapTransactionError; 
    }
    return ret;
}

bool SnapshotProxy::check_exist_snapshot()const
{
    return (!m_active_snapshot.empty() && m_exist_snapshot) ? true : false;
}

StatusCode SnapshotProxy::do_create(const SnapReqHead& shead, const string& sname)
{
    LOG_INFO << "do_create" << " snap_name:" << sname;

    ClientContext context;
    CreateReq ireq;
    ireq.mutable_header()->CopyFrom(shead);
    ireq.set_vol_name(m_vol_attr.vol_name());
    ireq.set_snap_name(sname);

    CreateAck iack;
    Status st = m_rpc_stub->Create(&context, ireq, &iack);
    if(!st.ok()){
        LOG_ERROR << "do_create" << " snap_name:" << sname << " failed";
        return iack.header().status();
    }
        
    LOG_INFO << "do_create" << " snap_name:" << sname << " ok";
    return StatusCode::sOk;
}

StatusCode SnapshotProxy::do_delete(const SnapReqHead& shead, const string& sname)
{
    LOG_INFO << "do_delete snap_name:" << sname;
    
    /*really tell dr server delete snapshot*/
    ClientContext context;
    DeleteReq ireq;
    ireq.mutable_header()->CopyFrom(shead);
    ireq.set_vol_name(m_vol_attr.vol_name());
    ireq.set_snap_name(sname);

    DeleteAck iack;
    Status st = m_rpc_stub->Delete(&context, ireq, &iack);
    if(!st.ok()){
        LOG_ERROR << "do_delete snap_name:" << sname << " failed";
        return iack.header().status();
    }
    LOG_INFO << "do_delete snap_name:" << sname << " ok";
    return StatusCode::sOk;
}

StatusCode SnapshotProxy::do_rollback(const SnapReqHead& shead, const string& sname)
{
    return StatusCode::sOk;
}

StatusCode SnapshotProxy::do_update(const SnapReqHead& shead, const string& sname, 
                                    const UpdateEvent& sevent)
{
    LOG_INFO << "do_update snap_name:" << sname;
    ClientContext context;
    UpdateReq ireq;
    ireq.mutable_header()->CopyFrom(shead);
    ireq.set_vol_name(m_vol_attr.vol_name());
    ireq.set_snap_name(sname);
    ireq.set_snap_event(sevent);
    UpdateAck iack;
    Status st = m_rpc_stub->Update(&context, ireq, &iack);

    /*whether update rpc ok or not, 
     * it will get current active snapshot from sg_server*/
    m_active_snapshot = iack.latest_snap_name();
    if(m_active_snapshot.empty()){
        m_exist_snapshot = false; 
    } else {
        m_exist_snapshot = true; 
    }

    if(!st.ok()){
        LOG_INFO << "do_update snap_name:" << sname << " failed";
        return iack.header().status();
    }
    
    LOG_INFO << "do_update snap_name:" << sname << " ok";
    return StatusCode::sOk;
}



void SnapshotProxy::split_cow_block(const off_t&  off, 
                                    const size_t& size,
                                    vector<cow_block_t>& cow_blocks)
{
    off_t  start = off;
    off_t  end   = off + size;
    size_t len   = size;
    block_t cur_blk_no;
    size_t  split_size;
    while(start < end && len > 0){
       if(start % COW_BLOCK_SIZE  == 0){
            cur_blk_no  = start / COW_BLOCK_SIZE;
            split_size  = min(len,  COW_BLOCK_SIZE);
        } else {
            cur_blk_no  = start / COW_BLOCK_SIZE;
            split_size  = min(len, ((cur_blk_no+1)*COW_BLOCK_SIZE) - start); 
        }
        cow_block_t cow_block;
        cow_block.off    = start;
        cow_block.len    = split_size;
        cow_block.blk_no = cur_blk_no;
        cow_blocks.push_back(cow_block);
        start += split_size;
        len   -= split_size;
    }
}

size_t SnapshotProxy::raw_device_write(char* buf, size_t len, off_t off)
{
    size_t left  = len;
    size_t write = 0;
    while(left > 0){
        int ret = pwrite(m_block_fd, buf+write, left, off+write);
        if(ret == -1 || ret == 0){
             LOG_ERROR << "pwrite fd:" << m_block_fd 
                       << " left:"  << left 
                       << " ret:"   << ret
                       << " errno:" << errno; 
            return ret;
        }
        left  -= ret;
        write += ret;
    }
    return len;
}

size_t SnapshotProxy::raw_device_read(char* buf, size_t len, off_t off)
{
    size_t left = len;
    size_t read = 0;
    while(left > 0){
        int ret = pread(m_block_fd, buf+read, left, off+read);
        if(ret == -1 || ret == 0){
            LOG_ERROR << "pread fd:" << m_block_fd 
                      << " left:" << left 
                      << " ret:"  << ret
                      << " errno:" << errno; 
            return ret;
        } 
        left -= ret;
        read += ret;
    }
    return read;
}

StatusCode SnapshotProxy::do_cow(const off_t& off, const size_t& size, char* buf, 
                                 bool rollback)
{
    vector<cow_block_t> cow_blocks;
    split_cow_block(off, size, cow_blocks);

    LOG_INFO << "do_cow snap_id:" << m_active_snapshot
             << " off:" << off << " size:" << size << " rollback:" << rollback;

    for(auto cow_block : cow_blocks){
        LOG_INFO << "do_cow off:" << cow_block.off  << " len:" << cow_block.len 
                 << " blk_no:" << cow_block.blk_no;

        /*todo: maintain bitmap or bloom filter to lookup block cow or overlap*/

        /*get cow meta from dr server*/ 
        ClientContext ctx1;
        Status status;
        CowReq cow_req;
        CowAck cow_ack;
        cow_req.set_vol_name(m_vol_attr.vol_name());
        cow_req.set_snap_name(m_active_snapshot);
        cow_req.set_blk_no(cow_block.blk_no);
        status = m_rpc_stub->CowOp(&ctx1, cow_req, &cow_ack);
        if(!status.ok()){
            return cow_ack.header().status();
        }
        /*cow or direct overlap*/
        if(cow_ack.op() == COW_NO){
            LOG_INFO << "do overlap"; 
            if(!rollback){
                /*comon io write, overlap*/
                char*   block_buf = buf + cow_block.off - off;
                off_t   block_off = cow_block.off;
                size_t  block_len = cow_block.len;
                size_t  write_ret = raw_device_write(block_buf, 
                                                     block_len, 
                                                     block_off);
                assert(write_ret == block_len);
            } else {
                /*being rollback, do nothing*/ 
            }
            continue;
        } 

        /*read from block device*/
        void* block_buf = nullptr;
        int ret = posix_memalign((void**)&block_buf, 512, COW_BLOCK_SIZE);
        assert(ret == 0 && block_buf != nullptr);
        off_t block_off = cow_block.blk_no * COW_BLOCK_SIZE;
        size_t read_ret = raw_device_read((char*)block_buf, 
                                          COW_BLOCK_SIZE, 
                                          block_off); 
        assert(read_ret == COW_BLOCK_SIZE);

        /*write to cow object*/
        string cow_object = cow_ack.cow_blk_object();
        ret = m_block_store->write(cow_object, (char*)block_buf, 
                                   COW_BLOCK_SIZE, 0);
        assert(ret == 0);
        free(block_buf);

        /*write new data to block device*/
        char*   cow_buf = buf + cow_block.off - off;
        off_t   cow_off = cow_block.off;
        size_t  cow_len = cow_block.len;
        size_t write_ret = raw_device_write(cow_buf, cow_len, cow_off);
        assert(write_ret == cow_len);

        /*update cow meta to dr server*/
        ClientContext ctx2;
        CowUpdateReq update_req;
        CowUpdateAck update_ack;
        update_req.set_vol_name(m_vol_attr.vol_name());
        update_req.set_snap_name(m_active_snapshot);
        update_req.set_blk_no(cow_block.blk_no);
        update_req.set_cow_blk_object(cow_object);
        status = m_rpc_stub->CowUpdate(&ctx2, update_req, &update_ack);
        if(!status.ok()){
            return update_ack.header().status();
        }
    }

    LOG_INFO << "do_cow snap_name:" << m_active_snapshot
             << " off:" << off << " size:" << size
             << " rollback:" << rollback << " ok";
    return StatusCode::sOk;
}

StatusCode SnapshotProxy::diff_snapshot(const DiffSnapshotReq* req, 
                                        DiffSnapshotAck* ack)
{
    string vname = req->vol_name();
    string first_snap_name = req->first_snap_name();
    string last_snap_name = req->last_snap_name();

    LOG_INFO << "diff_snapshot vname:" << vname
             << " first_snap:" << first_snap_name 
             << " last_snap:"  << last_snap_name;

    ClientContext context;
    DiffReq ireq;
    ireq.mutable_header()->CopyFrom(req->header());
    ireq.set_vol_name(vname);
    ireq.set_first_snap_name(first_snap_name);
    ireq.set_last_snap_name(last_snap_name);
    DiffAck iack;
    Status st = m_rpc_stub->Diff(&context, ireq, &iack);
    if(!st.ok()){
        return iack.header().status();
    }

    int diff_blocks_num = iack.diff_blocks_size();
    for(int i = 0; i < diff_blocks_num; i++){
        DiffBlocks  idiff_blocks= iack.diff_blocks(i); 
        DiffBlocks* odiff_blocks = ack->add_diff_blocks();
        odiff_blocks->CopyFrom(idiff_blocks);
    }

    LOG_INFO << "diff_snapshot vname:" << vname
             << " first_snap:" << first_snap_name
             << " last_snap:"  << last_snap_name
             << " ok";
    return StatusCode::sOk; 
}

StatusCode SnapshotProxy::read_snapshot(const ReadSnapshotReq* req, 
                                        ReadSnapshotAck* ack)
{
    string vname = req->vol_name();
    string sname = req->snap_name();
    off_t  off   = req->off();
    size_t len   = req->len();

    LOG_INFO << "read_snapshot vname:" << vname << " sname:" << sname 
             << " off:" << off << " len:" << len;

    ClientContext ctx0;
    ReadReq ireq;
    ireq.mutable_header()->CopyFrom(req->header());
    ireq.set_vol_name(vname);
    ireq.set_snap_name(sname);
    ireq.set_off(off);
    ireq.set_len(len);
    ReadAck iack;
    Status st = m_rpc_stub->Read(&ctx0, ireq, &iack);
    if(!st.ok()){
        return iack.header().status(); 
    }

    char* read_buf = nullptr;
    int ret = posix_memalign((void**)&read_buf, 512, len);
    assert(ret == 0 && read_buf != nullptr);

    /*--------first read----------------------*/
    interval_set<uint64_t> read_region; 
    read_region.insert(off, len);
    
    /*region read from orginal block device*/
    interval_set<uint64_t> read_device_region;
    read_device_region.insert(off, len);

    /*region read from cow object*/
    interval_set<uint64_t> read_cowobj_region;
    
    /*cow block set in first read*/
    set<uint64_t> cow_block_set0;

    int read_blk_num = iack.read_blocks_size(); 
    for(int i = 0; i < read_blk_num; i++){
        uint64_t block_no = iack.read_blocks(i).blk_no();
        string   block_object = iack.read_blocks(i).blk_object();
        
        /*accumulate record which cow block */
        cow_block_set0.insert(block_no);

        /*accumulate record which read from cow object*/
        read_cowobj_region.insert(block_no * COW_BLOCK_SIZE, COW_BLOCK_SIZE);

        interval_set<uint64_t> block_region;
        block_region.insert(block_no * COW_BLOCK_SIZE, COW_BLOCK_SIZE);
        block_region.intersection_of(read_region);

        /*block region read from cow object*/
        for(interval_set<uint64_t>::iterator it = block_region.begin();
            it != block_region.end(); it++){
            char*  rbuf = read_buf + it.get_start() - off;
            size_t rlen = it.get_len();
            off_t  roff = it.get_start() - (block_no * COW_BLOCK_SIZE);
            LOG_INFO << "read_snapshot first read cow object"
                     << " blk_no:" << block_no
                     << " blk_ob:" << block_object
                     << " cow_off:"  << it.get_start()
                     << " cow_len:"  << it.get_len();
            size_t read_ret = m_block_store->read(block_object, rbuf, rlen, roff);
            assert(read_ret == rlen);
        }
    }
    
    /*compute which read from block device*/
    read_device_region.subtract(read_cowobj_region);
    for(interval_set<uint64_t>::iterator it = read_device_region.begin();
        it != read_device_region.end(); it++){
        off_t  r_off = it.get_start();
        size_t r_len = it.get_len();
        char*  r_buf = read_buf + r_off - off;
        LOG_INFO << "read_snapshot first read block device"
                 << " cur_off:" << r_off
                 << " cur_len:" << r_len
                 << " align_off:" << ALIGN_UP(r_off, 512)
                 << " align_len:" << ALIGN_UP(r_len, 512);
        size_t r_ret = raw_device_read(r_buf, 
                                       ALIGN_UP(r_len, 512), 
                                       ALIGN_UP(r_off, 512)); 
        assert(r_ret == r_len);
    }

    /*----------second read---------------*/
    ClientContext ctx1;
    ReadReq ireq1;
    ireq1.mutable_header()->CopyFrom(req->header());
    ireq1.set_vol_name(vname);
    ireq1.set_snap_name(sname);
    ireq1.set_off(off);
    ireq1.set_len(len);
    ReadAck iack1;
    st = m_rpc_stub->Read(&ctx1, ireq1, &iack1);
    
    int read_block_num1 = iack1.read_blocks_size();
    for(int i = 0; i < read_block_num1; i++){
        uint64_t block_no = iack1.read_blocks(i).blk_no();
        string   block_object = iack1.read_blocks(i).blk_object();
        
        /*cow block has read during first second*/
        if(cow_block_set0.find(block_no) != cow_block_set0.end()){
            continue;
        }
        
        /*when second read, some region in first read from block deivce should
         *read from new snapshot cow object*/
        interval_set<uint64_t> block_region;
        block_region.insert(block_no * COW_BLOCK_SIZE, COW_BLOCK_SIZE);
        block_region.intersection_of(read_device_region);

        /*block region read from cow object*/
        for(interval_set<uint64_t>::iterator it = block_region.begin();
            it != block_region.end(); it++){
            char*  rbuf = read_buf + it.get_start() - off;
            size_t rlen = it.get_len();
            off_t  roff = it.get_start() - (block_no * COW_BLOCK_SIZE);

            LOG_INFO << "read_snapshot second read cow object"
                     << " blk_no:" << block_no
                     << " blk_ob:" << block_object
                     << " cow_off:"  << it.get_start()
                     << " cow_len:"  << it.get_len();
 
            m_block_store->read(block_object, rbuf, rlen, roff);
        }
    }
    
    ack->mutable_header()->set_status(StatusCode::sOk);
    ack->set_data(read_buf, len);
    if(read_buf){
        free(read_buf);
    }

    LOG_INFO << "read_snapshot vname:" << vname << " sname:" << sname 
             << " off:" << off << " len:" << len 
             << " data size:" << ack->data().size() 
             << " data_len:"  << ack->data().length() << " ok";
    return StatusCode::sOk; 
}