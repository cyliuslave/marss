
/*
 * MARSSx86 : A Full System Computer-Architecture Simulator
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Copyright 2009 Avadh Patel <apatel@cs.binghamton.edu>
 * Copyright 2009 Furat Afram <fafram@cs.binghamton.edu>
 *
 */

#ifdef MEM_TEST
#include <test.h>
#else
#include <ptlsim.h>
#define PTLSIM_PUBLIC_ONLY
#include <ptlhwdef.h>
#endif

#include <memoryHierarchy.h>
#include <coherentCache.h>
#include <mesiLogic.h>

#include <machine.h>

using namespace Memory;
using namespace Memory::CoherentCache;


CacheController::CacheController(W8 coreid, const char *name,
        MemoryHierarchy *memoryHierarchy, CacheType type) :
    Controller(coreid, name, memoryHierarchy)
    , type_(type)
    , isLowestPrivate_(false)
    , directory_(NULL)
    , lowerCont_(NULL)
    , coherence_logic_(NULL)
{
    memoryHierarchy_->add_cache_mem_controller(this);
    new_stats = new MESIStats(name, &memoryHierarchy->get_machine());

    readaccesses = 0, readmisses = 0, conflicts = 0;
    writeaccesses = 0, writemisses = 0;

    cacheLines_ = get_cachelines(type);

    if(!memoryHierarchy_->get_machine().get_option(name, "last_private", isLowestPrivate_)) {
        isLowestPrivate_ = false;
    }

    cacheLineBits_ = cacheLines_->get_line_bits();
    cacheAccessLatency_ = cacheLines_->get_access_latency();

    cacheLines_->init();


    SET_SIGNAL_CB(name, "_Cache_Hit", cacheHit_, &CacheController::cache_hit_cb);

    SET_SIGNAL_CB(name, "_Cache_Miss", cacheMiss_, &CacheController::cache_miss_cb);

    SET_SIGNAL_CB(name, "_Cache_Insert", cacheInsert_, &CacheController::cache_insert_cb);

    SET_SIGNAL_CB(name, "_Cache_Update", cacheUpdate_, &CacheController::cache_update_cb);

    SET_SIGNAL_CB(name, "_Wait_Interconnect", waitInterconnect_,
            &CacheController::wait_interconnect_cb);

    SET_SIGNAL_CB(name, "_Cache_Access", cacheAccess_, &CacheController::cache_access_cb);

    SET_SIGNAL_CB(name, "_Clear_Entry", clearEntry_, &CacheController::clear_entry_cb);

    SET_SIGNAL_CB(name, "_Cache_Insert_Complete", cacheInsertComplete_,
            &CacheController::cache_insert_complete_cb);

    upperInterconnect_  = NULL;
    upperInterconnect2_ = NULL;
    lowerInterconnect_  = NULL;

    new_stats->cpurequest.count.miss.write.enable_periodic_dump();
    new_stats->cpurequest.count.miss.read.enable_periodic_dump();

}

CacheController::~CacheController()
{
    delete new_stats;
}

CacheQueueEntry* CacheController::find_dependency(MemoryRequest *request)
{
    W64 requestLineAddress = get_line_address(request);

    CacheQueueEntry* queueEntry;
    foreach_list_mutable(pendingRequests_.list(), queueEntry, entry,
            prevEntry) {

        if(request == queueEntry->request || queueEntry->annuled)
            continue;

        if(get_line_address(queueEntry->request) == requestLineAddress) {
            /*
             * Found an entry with same line address, check if other
             * entry also depends on this entry or not and to
             * maintain a chain of dependent entries, return the
             * last entry in the chain
             */
            while(queueEntry->depends >= 0)
                queueEntry = &pendingRequests_[queueEntry->depends];

            return queueEntry;
        }
    }
    return NULL;
}

bool CacheController::is_line_in_use(W64 tag)
{
    if (tag == InvalidTag<W64>::INVALID || tag == (W64)-1) {
        return false;
    }

    /* Check each local cache request for same line tag */
    CacheQueueEntry* queueEntry;
    foreach_list_mutable(pendingRequests_.list(), queueEntry, entry,
            prevEntry) {
        if (get_line_address(queueEntry->request) == tag) {
            return true;
        }
    }

    return false;
}

CacheQueueEntry* CacheController::find_match(MemoryRequest *request)
{
    CacheQueueEntry* queueEntry;
    foreach_list_mutable(pendingRequests_.list(), queueEntry, entry,
            prevEntry) {
        if(request == queueEntry->request)
            return queueEntry;
    }

    return NULL;
}

void CacheController::print(ostream& os) const
{
    os << "---Cache-Controller: " << get_name() << endl;
    if(pendingRequests_.count() > 0)
        os << "Queue : " << pendingRequests_ << endl;
    os << "---End Cache-Controller : " << get_name() << endl;
}

bool CacheController::handle_upper_interconnect(Message &message)
{
    memdebug(get_name() <<
            " Received message from upper interconnect\n");

    /* set full flag if buffer is full */
    if(is_full()) {
        memoryHierarchy_->set_controller_full(this, true);
        return false;
    }

    CacheQueueEntry *queueEntry = pendingRequests_.alloc();

    if(queueEntry == NULL) {
        return false;
    }

    queueEntry->request = message.request;
    queueEntry->sender  = (Interconnect*)message.sender;
    queueEntry->isSnoop = false;
    queueEntry->m_arg   = message.arg;
    queueEntry->source  = (Controller*)message.origin;
    queueEntry->dest    = (Controller*)message.dest;
    queueEntry->request->incRefCounter();

    queueEntry->eventFlags[CACHE_ACCESS_EVENT]++;

    /* Check dependency and access the cache */
    CacheQueueEntry* dependsOn = find_dependency(message.request);

    if(dependsOn) {
        /* Found an dependency */
        memdebug("dependent entry: " << *dependsOn << endl);
        dependsOn->depends = queueEntry->idx;
        queueEntry->waitFor = dependsOn->idx;
        OP_TYPE type       = queueEntry->request->get_type();
        bool kernel_req    = queueEntry->request->is_kernel();
        if(type == MEMORY_OP_READ) {
            N_STAT_UPDATE(new_stats->cpurequest.stall.read.dependency, ++, kernel_req);
        } else if(type == MEMORY_OP_WRITE) {
            N_STAT_UPDATE(new_stats->cpurequest.stall.write.dependency, ++, kernel_req);
        }
    } else {
        cache_access_cb(queueEntry);
    }

    ADD_HISTORY_ADD(queueEntry->request);

    memdebug("Cache: " << get_name() << " added queue entry: " <<
            *queueEntry << endl);
    return true;
}

bool CacheController::handle_lower_interconnect(Message &message)
{
    memdebug(get_name() <<
            " Received message from lower interconnect\n");

    CacheQueueEntry *queueEntry = find_match(message.request);
    if(queueEntry) queueEntry->m_arg = message.arg;

    if (queueEntry && queueEntry->annuled)
        return true;

    if(queueEntry != NULL && !queueEntry->isSnoop &&
            queueEntry->request == message.request) {
        if(message.hasData) {
            return complete_request(message, queueEntry);
        } else if (isLowestPrivate_){
            coherence_logic_->handle_response(queueEntry, message);
            return true;
        }
    }

    if(isLowestPrivate_) {

        /* Ignore response that is not for this controller */
        if (message.hasData)
            return true;

        CacheQueueEntry *newEntry = pendingRequests_.alloc();
        assert(newEntry);
        newEntry->request = message.request;
        newEntry->isSnoop = true;
        newEntry->sender  = (Interconnect*)message.sender;
        newEntry->source  = (Controller*)message.origin;
        newEntry->dest    = (Controller*)message.dest;
        newEntry->request->incRefCounter();

        newEntry->eventFlags[CACHE_ACCESS_EVENT]++;
        marss_add_event(&cacheAccess_, 0,
                newEntry);
        ADD_HISTORY_ADD(newEntry->request);
    } else { // not lowestPrivate cache
        if(queueEntry == NULL) {
            /* check if request is cache eviction */
            if(message.request->get_type() == MEMORY_OP_EVICT ||
                    message.request->get_type() == MEMORY_OP_UPDATE) {
                /* alloc new queueentry and evict the cache line if present */
                CacheQueueEntry *evictEntry = pendingRequests_.alloc();
                assert(evictEntry);

                evictEntry->request = message.request;
                evictEntry->request->incRefCounter();
                evictEntry->isSnoop = true;
                evictEntry->m_arg   = message.arg;
                evictEntry->eventFlags[CACHE_ACCESS_EVENT]++;
                marss_add_event(&cacheAccess_, 1,
                        evictEntry);
                ADD_HISTORY_ADD(evictEntry->request);
            }
        }
    }
    return true;

}

bool CacheController::is_line_valid(CacheLine *line)
{
    return coherence_logic_->is_line_valid(line);
}

void CacheController::send_message(CacheQueueEntry *queueEntry,
        Interconnect *interconn, OP_TYPE type, W64 tag)
{
    MemoryRequest *request = memoryHierarchy_->get_free_request(
            queueEntry->request->get_coreid());
    assert(request);

    if(tag == InvalidTag<W64>::INVALID || tag == (W64)-1)
        tag = queueEntry->request->get_physical_address();

    request->init(queueEntry->request);
    request->set_physical_address(tag);
    request->set_op_type(type);

    CacheQueueEntry *evictEntry = pendingRequests_.alloc();
    assert(evictEntry);

    /* set full flag if buffer is full */
    if(pendingRequests_.isFull()) {
        memoryHierarchy_->set_controller_full(this, true);
    }

    evictEntry->request = request;
    evictEntry->sender  = NULL;
    evictEntry->sendTo  = interconn;
    evictEntry->dest    = queueEntry->dest;
    evictEntry->line    = queueEntry->line;
    evictEntry->request->incRefCounter();

    //memdebug("Created Evict message: ", *evictEntry, endl);
    ADD_HISTORY_ADD(evictEntry->request);

    evictEntry->eventFlags[CACHE_WAIT_INTERCONNECT_EVENT]++;
    marss_add_event(&waitInterconnect_, 1, evictEntry);
}

void CacheController::send_evict_to_upper(CacheQueueEntry *entry, W64 oldTag)
{
    send_message(entry, upperInterconnect_, MEMORY_OP_EVICT, oldTag);

    if(upperInterconnect2_)
        send_message(entry, upperInterconnect2_, MEMORY_OP_EVICT, oldTag);
}

void CacheController::send_evict_to_lower(CacheQueueEntry *entry, W64 oldTag)
{
    send_message(entry, lowerInterconnect_, MEMORY_OP_EVICT, oldTag);
}

void CacheController::send_update_to_upper(CacheQueueEntry *entry, W64 tag)
{
    send_message(entry, upperInterconnect_, MEMORY_OP_UPDATE, tag);

    if(upperInterconnect2_)
        send_message(entry, upperInterconnect2_, MEMORY_OP_UPDATE, tag);
}

void CacheController::send_update_to_lower(CacheQueueEntry *entry, W64 tag)
{
    entry->dest = lowerCont_;
    send_message(entry, lowerInterconnect_, MEMORY_OP_UPDATE, tag);
}

void CacheController::handle_cache_insert(CacheQueueEntry *queueEntry,
        W64 oldTag)
{
    coherence_logic_->handle_cache_insert(queueEntry, oldTag);
}

bool CacheController::complete_request(Message &message,
        CacheQueueEntry *queueEntry)
{
    if (pendingRequests_.count() >= (
                pendingRequests_.size() - 4)) {
        return false;
    }

    /*
     * first check that we have a valid line pointer in queue entry
     * and then check that message has data flag set
     */
    if(queueEntry->line == NULL || queueEntry->line->tag !=
            cacheLines_->tagOf(queueEntry->request->get_physical_address())) {
        W64 oldTag = InvalidTag<W64>::INVALID;
        CacheLine *line = cacheLines_->insert(queueEntry->request,
                oldTag);

        /* If line is in use then don't evict it, it will be inserted later. */
        if (is_line_in_use(oldTag)) {
            oldTag = -1;
        }

        queueEntry->line = line;
        handle_cache_insert(queueEntry, oldTag);
        queueEntry->line->init(cacheLines_->tagOf(queueEntry->
                    request->get_physical_address()));
    }

    assert(queueEntry->line);
    assert(message.hasData);

    coherence_logic_->complete_request(queueEntry, message);

    /* insert the updated line into cache */
    queueEntry->eventFlags[CACHE_INSERT_EVENT]++;
    marss_add_event(&cacheInsert_, 0,
            (void*)(queueEntry));

    /* send back the response */
    queueEntry->sendTo = queueEntry->sender;
    marss_add_event(&waitInterconnect_, 1, queueEntry);

    memdebug("Cache Request completed: " << *queueEntry << endl);

    return true;
}

bool CacheController::handle_interconnect_cb(void *arg)
{
    Message *msg = (Message*)arg;
    Interconnect *sender = (Interconnect*)msg->sender;

    memdebug("Message received is: " << *msg);
    //printf("Inside coherence cache cb!\n");

    if(sender == upperInterconnect_ || sender == upperInterconnect2_) {
        return handle_upper_interconnect(*msg);
    } else {
        return handle_lower_interconnect(*msg);
    }

    return true;
}

int CacheController::access_fast_path(Interconnect *interconnect,
        MemoryRequest *request)
{
    memdebug("Accessing Cache " << get_name() << " : Request: " << *request << endl);
    CacheLine *line	= NULL;

    if (find_dependency(request) != NULL) {
        return -1;
    }

    if (request->get_type() != MEMORY_OP_WRITE)
        line = cacheLines_->probe(request);

    //if (request->get_coreid() == 1)
    //    printf("access fast path: line = %p\n", line);

    /*
     * if its a write, dont do fast access as the lower
     * level cache has to be updated
     */
    if(line && is_line_valid(line) &&
            request->get_type() != MEMORY_OP_WRITE) {
        N_STAT_UPDATE(new_stats->cpurequest.count.hit.read.hit, ++,
                request->is_kernel());
        return cacheLines_->latency();
    }

    return -1;
}

void CacheController::print_map(ostream& os)
{
    os << "Cache-Controller: " << get_name() << endl;
    os << "\tconnected to: " << endl;
    if(upperInterconnect_)
        os << "\t\tupper: " << upperInterconnect_->get_name() << endl;
    if(upperInterconnect2_)
        os << "\t\tupper2: " << upperInterconnect2_->get_name() << endl;
    if(lowerInterconnect_)
        os << "\t\tlower: " <<  lowerInterconnect_->get_name() << endl;
}

void CacheController::register_interconnect(Interconnect *interconnect,
        int type)
{
    switch(type) {
        case INTERCONN_TYPE_UPPER:
            upperInterconnect_ = interconnect;
            break;
        case INTERCONN_TYPE_UPPER2:
            upperInterconnect2_ = interconnect;
            break;
        case INTERCONN_TYPE_LOWER:
            lowerInterconnect_ = interconnect;
            get_directory(interconnect);
            break;
        default:
            assert(0);
    }
}

void CacheController::get_directory(Interconnect *interconn)
{
    BaseMachine &machine = memoryHierarchy_->get_machine();
    foreach (i, machine.connections.count()) {
        ConnectionDef *conn_def = machine.connections[i];

        if (strcmp(conn_def->name.buf, interconn->get_name()) == 0) {
            foreach (j, conn_def->connections.count()) {
                SingleConnection *sg = conn_def->connections[j];
                Controller **cont = NULL;

                switch (sg->type) {
                    case INTERCONN_TYPE_DIRECTORY:
                        cont = machine.controller_hash.get(
                                sg->controller);
                        assert(cont);
                        directory_ = *cont;
                        break;
                    case INTERCONN_TYPE_UPPER:
                        cont = machine.controller_hash.get(
                                sg->controller);
                        assert(cont);
                        lowerCont_ = *cont;
                        break;
                    default:
                        break;
                }
            }
        }
    }
}

void CacheController::register_upper_interconnect(Interconnect *interconnect)
{
    upperInterconnect_ = interconnect;
}

void CacheController::register_second_upper_interconnect(Interconnect
        *interconnect)
{
    upperInterconnect2_ = interconnect;
}

void CacheController::register_lower_interconnect(Interconnect *interconnect)
{
    lowerInterconnect_ = interconnect;
}

bool CacheController::check_cache_states_bit(W8 value1, W8 value2){
    return cacheLines_->check_lines_states_tsx(value1 , value2);
}

void CacheController::reset_cache_states_bit(W8 value){
    cacheLines_->reset_lines_states(value);
}

bool CacheController::cache_hit_cb(void *arg)
{
    CacheQueueEntry *queueEntry = (CacheQueueEntry*)arg;
    if(queueEntry->annuled)
        return true;

    queueEntry->eventFlags[CACHE_HIT_EVENT]--;

    if(queueEntry->isSnoop) {
        if (pendingRequests_.count() >=  (
                    pendingRequests_.size() - 4)) {
            /* Snoop hit can cause eviction in local cache and if we dont have
             * free queue entries we delay this by 2 cycles */
            marss_add_event(&cacheHit_, 2, queueEntry);
        } else {
            coherence_logic_->handle_interconn_hit(queueEntry);
        }
    } else {
        coherence_logic_->handle_local_hit(queueEntry);
    }

    return true;
}

bool CacheController::cache_miss_cb(void *arg)
{
    CacheQueueEntry *queueEntry = (CacheQueueEntry*)arg;
    if(queueEntry->annuled)
        return true;

    queueEntry->eventFlags[CACHE_MISS_EVENT]--;

    if(queueEntry->request->get_type() == MEMORY_OP_EVICT &&
            !is_lowest_private()) {
        if(queueEntry->line)
            coherence_logic_->invalidate_line(queueEntry->line);
        clear_entry_cb(queueEntry);
        return true;
    }

    memdebug("Cache Miss: " << *queueEntry << endl);

    if(queueEntry->isSnoop) {
        /*
         * make sure that line pointer is NULL so when
         * message is sent to interconnect it will make
         * shared flag to false
         */
        queueEntry->line         = NULL;
        queueEntry->isShared     = false;
        queueEntry->responseData = false;
        coherence_logic_->handle_interconn_miss(queueEntry);
    } else {
        coherence_logic_->handle_local_miss(queueEntry);
    }

    return true;
}

bool CacheController::cache_update_cb(void *arg)
{
    assert(0);
    return false;
}

bool CacheController::cache_insert_cb(void *arg)
{
    CacheQueueEntry *queueEntry = (CacheQueueEntry*)arg;
    if(queueEntry->annuled)
        return true;

    queueEntry->eventFlags[CACHE_INSERT_EVENT]--;
    assert(queueEntry->line);

    if(cacheLines_->get_port(queueEntry->request)) {

        queueEntry->eventFlags[CACHE_INSERT_COMPLETE_EVENT]++;
        marss_add_event(&cacheInsertComplete_,
                cacheAccessLatency_, queueEntry);
        return true;
    }

    queueEntry->eventFlags[CACHE_INSERT_EVENT]++;
    marss_add_event(&cacheInsert_, 1,
            (void*)(queueEntry));
    return true;
}

bool CacheController::cache_insert_complete_cb(void *arg)
{
    CacheQueueEntry *queueEntry = (CacheQueueEntry*)arg;
    if(queueEntry->annuled)
        return true;

    queueEntry->eventFlags[CACHE_INSERT_COMPLETE_EVENT]--;

    queueEntry->eventFlags[CACHE_CLEAR_ENTRY_EVENT]++;
    marss_add_event(&clearEntry_,
            0, queueEntry);

    return true;
}

bool CacheController::cache_access_cb(void *arg)
{
    CacheQueueEntry *queueEntry = (CacheQueueEntry*)arg;
    if(queueEntry->annuled)
        return true;

    queueEntry->eventFlags[CACHE_ACCESS_EVENT]--;
    bool kernel_req = queueEntry->request->is_kernel();
	OP_TYPE type = queueEntry->request->get_type();

    if(cacheLines_->get_port(queueEntry->request)) {
        bool hit;
        CacheLine *line	= cacheLines_->probe(queueEntry->request);
        queueEntry->line = line;

        if(line) hit = true;
        else hit = false;

        //if (queueEntry->request->get_coreid() == 1)
        //    printf("Inside coherent cache %s hit = %d\n", get_name(), hit);

        // Testing 100 % L2 Hit
        // if(type_ == L2_CACHE)
        // hit = true;

        Signal *signal;
        int delay;
        if(hit) {
            signal = &cacheHit_;
            delay = cacheAccessLatency_;

			if (!queueEntry->isSnoop) {
				if(type == MEMORY_OP_READ) {
					N_STAT_UPDATE(new_stats->cpurequest.count.hit.read.hit, ++,
							kernel_req);
				} else if(type == MEMORY_OP_WRITE) {
					N_STAT_UPDATE(new_stats->cpurequest.count.hit.write.hit, ++,
							kernel_req);
				}
			}
        } else { // Cache Miss
            signal = &cacheMiss_;
            delay = cacheAccessLatency_;

            N_STAT_UPDATE(new_stats->miss_state.cpu, [4]++,
                    kernel_req);

			if (!queueEntry->isSnoop) {
				if(type == MEMORY_OP_READ) {
					N_STAT_UPDATE(new_stats->cpurequest.count.miss.read, ++,
							kernel_req);
				} else if(type == MEMORY_OP_WRITE) {
					N_STAT_UPDATE(new_stats->cpurequest.count.miss.write, ++,
							kernel_req);
				}
			}
        }
        marss_add_event(signal, delay,
                (void*)queueEntry);
        return true;
    } else {
        OP_TYPE type = queueEntry->request->get_type();
        if(type == MEMORY_OP_READ) {
            N_STAT_UPDATE(new_stats->cpurequest.stall.read.cache_port, ++,
                    kernel_req);
        } else if(type == MEMORY_OP_WRITE) {
            N_STAT_UPDATE(new_stats->cpurequest.stall.write.cache_port, ++,
                    kernel_req);
        }
    }

    /* No port available yet, retry next cycle */
    queueEntry->eventFlags[CACHE_ACCESS_EVENT]++;
    marss_add_event(&cacheAccess_, 1, arg);

    return true;
}

bool CacheController::wait_interconnect_cb(void *arg)
{
    CacheQueueEntry *queueEntry = (CacheQueueEntry*)arg;
    if(queueEntry->annuled)
        return true;

    queueEntry->eventFlags[CACHE_WAIT_INTERCONNECT_EVENT]--;

    if(queueEntry->sendTo == NULL)
        ptl_logfile << "Queueentry: " << *queueEntry << endl;
    assert(queueEntry->sendTo);

    memdebug("Queue Entry: " << *queueEntry << endl);

    Message& message = *memoryHierarchy_->get_message();
    message.sender   = this;
    message.request  = queueEntry->request;
    message.dest     = queueEntry->dest;
    bool success     = false;

    if (queueEntry->line) message.arg = &(queueEntry->line->state);
    else message.arg = NULL;

    if(queueEntry->sendTo == upperInterconnect_ ||
            queueEntry->sendTo == upperInterconnect2_) {
        /*
         * sending to upper interconnect, so its a response to
         * previous request, so mark 'hasData' to true in message
         */
        message.hasData = true;
        memdebug("Sending message: " << message << endl);
        success = queueEntry->sendTo->get_controller_request_signal()->
            emit(&message);

        if(success == true) {
            /* free this entry if no future event is going to use it */
            clear_entry_cb(queueEntry);
        } else {
            /* Queue in interconnect is full so retry after interconnect delay */
            queueEntry->eventFlags[CACHE_WAIT_INTERCONNECT_EVENT]++;
            marss_add_event(&waitInterconnect_,
                    queueEntry->sendTo->get_delay(), (void*)queueEntry);
        }
    } else {
        if(queueEntry->request->get_type() == MEMORY_OP_UPDATE ||
                queueEntry->responseData)
            message.hasData = true;
        else
            message.hasData = false;

        message.isShared = queueEntry->isShared;

        success = lowerInterconnect_->
            get_controller_request_signal()->emit(&message);

        memdebug("success is : " << success << endl);
        if(success == false) {
            /* Queue in interconnect full so retry after interconnect delay */
            int delay = lowerInterconnect_->get_delay();
            if(delay == 0) delay = AVG_WAIT_DELAY;
            queueEntry->eventFlags[CACHE_WAIT_INTERCONNECT_EVENT]++;
            marss_add_event(&waitInterconnect_,
                    delay, (void*)queueEntry);
        } else {
            /*
             * If the request is for memory update or snoop, send to
             * lower level cache so we can remove the entry from
             * local queue
             */
            if(queueEntry->request->get_type() == MEMORY_OP_UPDATE ||
                    queueEntry->request->get_type() == MEMORY_OP_EVICT ||
                    queueEntry->isSnoop) {
                clear_entry_cb(queueEntry);
            }
        }
    }

    /* Free the message */
    memoryHierarchy_->free_message(&message);

    return true;
}

bool CacheController::clear_entry_cb(void *arg)
{
    CacheQueueEntry *queueEntry = (CacheQueueEntry*)arg;
    if(queueEntry->annuled)
        return true;

    if(queueEntry->free)
        return true;

    queueEntry->eventFlags[CACHE_CLEAR_ENTRY_EVENT]--;

    memdebug("Queue Entry flags: " << queueEntry->eventFlags << endl);
    if(queueEntry->eventFlags.iszero()) {

        /* Get the dependent entry if any */
        if(queueEntry->depends >= 0) {
            CacheQueueEntry* depEntry = &pendingRequests_[
                queueEntry->depends];
            depEntry->waitFor = -1;
            marss_add_event(&cacheAccess_, 1, depEntry);
        }

        queueEntry->request->decRefCounter();
        ADD_HISTORY_REM(queueEntry->request);
        if(!queueEntry->annuled) {
            if(pendingRequests_.list().count == 0) {
                memdebug("Removing from pending request queue " <<
                        pendingRequests_ << " \nQueueEntry: " <<
                        queueEntry << endl);
            }

            pendingRequests_.free(queueEntry);
        }

        /*
         * Check if pendingRequests_ buffer is not full then
         * clear the flag in memory hierarchy
         */
        if(!pendingRequests_.isFull()) {
            memoryHierarchy_->set_controller_full(this, false);
        }
    }

    return true;
}

void CacheController::annul_request(MemoryRequest *request)
{
    CacheQueueEntry *queueEntry;
    foreach_list_mutable(pendingRequests_.list(), queueEntry,
            entry, nextentry) {
        if (queueEntry->request->is_same(request)) {
            queueEntry->annuled = true;
            /* Fix dependency chain if this entry was waiting for
             * some other entry, else wakeup that entry.*/
            if(queueEntry->depends >= 0) {
                CacheQueueEntry* depEntry = &pendingRequests_[
                    queueEntry->depends];
                if (queueEntry->waitFor >= 0) {
                    depEntry->waitFor = queueEntry->waitFor;
                    pendingRequests_[queueEntry->waitFor].depends =
                        depEntry->idx;
                } else {
                    depEntry->waitFor = -1;
                    marss_add_event(&cacheAccess_, 1, depEntry);
                }
            } else if (queueEntry->waitFor >= 0) {
                pendingRequests_[queueEntry->waitFor].depends = -1;
            }

            pendingRequests_.free(queueEntry);
            ADD_HISTORY_REM(queueEntry->request);

            queueEntry->request->decRefCounter();
        }
    }
}

CacheQueueEntry* CacheController::get_new_queue_entry()
{
    CacheQueueEntry *queueEntry = pendingRequests_.alloc();
    assert(queueEntry);

    return queueEntry;
}

/**
 * @brief Dump Coherent Cache Configuration in YAML Format
 *
 * @param out YAML Object
 */
void CacheController::dump_configuration(YAML::Emitter &out) const
{
	out << YAML::Key << get_name() << YAML::Value << YAML::BeginMap;

	YAML_KEY_VAL(out, "type", "cache");
	YAML_KEY_VAL(out, "size", cacheLines_->get_size());
	YAML_KEY_VAL(out, "sets", cacheLines_->get_set_count());
	YAML_KEY_VAL(out, "ways", cacheLines_->get_way_count());
	YAML_KEY_VAL(out, "line_size", cacheLines_->get_line_size());
	YAML_KEY_VAL(out, "latency", cacheLines_->get_access_latency());
	YAML_KEY_VAL(out, "pending_queue_size", pendingRequests_.size());

	coherence_logic_->dump_configuration(out);

	out << YAML::EndMap;
}

void CacheController::dump_mcpat_configuration(root_system *mcpat, W32 core)
{
        if (( strstr(get_name(), "L1_I") !=NULL) || ( strstr(get_name(), "L1_I_A") !=NULL) ) {
                mcpat->core[idx].icache.icache_config[0] = cacheLines_->get_size();
                mcpat->core[idx].icache.icache_config[1] = cacheLines_->get_line_size();
                mcpat->core[idx].icache.icache_config[2] = cacheLines_->get_way_count();
                mcpat->core[idx].icache.icache_config[3] = 1;
                mcpat->core[idx].icache.icache_config[4] = 8;
                mcpat->core[idx].icache.icache_config[5] = cacheLines_->get_access_latency();
                mcpat->core[idx].icache.icache_config[6] = 1;
/* The below values need verification */
                mcpat->core[idx].icache.buffer_sizes[0] = 16;
                mcpat->core[idx].icache.buffer_sizes[1] = 16;
                mcpat->core[idx].icache.buffer_sizes[2] = pendingRequests_.size();
                mcpat->core[idx].icache.buffer_sizes[3] = 0;
        }

        if (( strstr(get_name(), "L1_D") !=NULL) || ( strstr(get_name(), "L1_D_A") !=NULL) ) {
                mcpat->core[idx].dcache.dcache_config[0] = cacheLines_->get_size();
                mcpat->core[idx].dcache.dcache_config[1] = cacheLines_->get_line_size();
                mcpat->core[idx].dcache.dcache_config[2] = cacheLines_->get_way_count();
                mcpat->core[idx].dcache.dcache_config[3] = 1;
                mcpat->core[idx].dcache.dcache_config[4] = 3;
                mcpat->core[idx].dcache.dcache_config[5] = cacheLines_->get_access_latency();
                mcpat->core[idx].dcache.dcache_config[6] = 1;
                mcpat->core[idx].dcache.buffer_sizes[0] = 16;
                mcpat->core[idx].dcache.buffer_sizes[1] = 16;
                mcpat->core[idx].dcache.buffer_sizes[2] = pendingRequests_.size();
                mcpat->core[idx].dcache.buffer_sizes[3] = 16;
        }
        if (( strstr(get_name(), "L2") !=NULL) ) {
                int count = core;
                mcpat->L2[count].L2_config[0] = cacheLines_->get_size();
                mcpat->L2[count].L2_config[1] = cacheLines_->get_line_size();
                mcpat->L2[count].L2_config[2] = cacheLines_->get_way_count();
                mcpat->L2[count].L2_config[3] = 1;
                mcpat->L2[count].L2_config[4] = 3;
                mcpat->L2[count].L2_config[5] = cacheLines_->get_access_latency();
                mcpat->L2[count].L2_config[6] = 1;
                mcpat->L2[count].buffer_sizes[0] = 16;
                mcpat->L2[count].buffer_sizes[1] = 16;
                mcpat->L2[count].buffer_sizes[2] = pendingRequests_.size();
                mcpat->L2[count].buffer_sizes[3] = 16;
                mcpat->L2[count].duty_cycle = 1;
        }
}

void CacheController::reset_lastcycle_stats()
{
        new_stats->cpurequest.count.miss.read(user_stats) = readmiss_user;
	new_stats->cpurequest.count.miss.read(kernel_stats) = readmiss_kernel;
	new_stats->cpurequest.count.hit.read.hit(user_stats) = readhit_user;
	new_stats->cpurequest.count.hit.read.hit(kernel_stats) = readhit_kernel;
	new_stats->cpurequest.count.hit.read.forward(user_stats) = readhit_forward_user;
	new_stats->cpurequest.count.hit.read.forward(kernel_stats) = readhit_forward_kernel;
        new_stats->cpurequest.count.miss.write(user_stats) = writemiss_user;
	new_stats->cpurequest.count.miss.write(kernel_stats) = writemiss_kernel;
	new_stats->cpurequest.count.hit.write.hit(user_stats) = writehit_user;
	new_stats->cpurequest.count.hit.write.hit(kernel_stats) = writehit_kernel;
	new_stats->cpurequest.count.hit.write.forward(user_stats) = writehit_forward_user;
	new_stats->cpurequest.count.hit.write.forward(kernel_stats) = writehit_forward_kernel;
	new_stats->cpurequest.stall.read.dependency(user_stats) = stall_rdepend_user;
	new_stats->cpurequest.stall.read.dependency(kernel_stats) = stall_rdepend_kernel;
	new_stats->cpurequest.stall.read.cache_port(user_stats) = stall_rcachep_user;
	new_stats->cpurequest.stall.read.cache_port(kernel_stats) = stall_rcachep_kernel;
	new_stats->cpurequest.stall.write.dependency(user_stats) = stall_wdepend_user;
	new_stats->cpurequest.stall.write.dependency(kernel_stats) = stall_wdepend_kernel;
	new_stats->cpurequest.stall.write.cache_port(user_stats) = stall_wcachep_user;
	new_stats->cpurequest.stall.write.cache_port(kernel_stats) = stall_wcachep_kernel;
	new_stats->cpurequest.stall.read.buffer_full(user_stats) = stall_rbufferfull_user;
	new_stats->cpurequest.stall.read.buffer_full(kernel_stats) = stall_rbufferfull_kernel;
	new_stats->cpurequest.stall.write.buffer_full(user_stats) = stall_wbufferfull_user;
	new_stats->cpurequest.stall.write.buffer_full(kernel_stats) = stall_wbufferfull_kernel;
}

void CacheController::dump_mcpat_stats(root_system *mcpat, W32 coreid)
{
	system_core *core = &(mcpat->core[idx]);

        W64 rmiss_user = (new_stats->cpurequest.count.miss.read)(user_stats) - 0;
        W64 rmiss_kernel = (new_stats->cpurequest.count.miss.read)(kernel_stats) - 0;
        W64 rhit_user = (new_stats->cpurequest.count.hit.read.hit)(user_stats) - 0;
        W64 rhit_kernel = (new_stats->cpurequest.count.hit.read.hit)(kernel_stats) - 0;
	W64 rhit_forward_user = (new_stats->cpurequest.count.hit.read.forward)(user_stats) - 0;
	W64 rhit_forward_kernel = (new_stats->cpurequest.count.hit.read.forward)(kernel_stats) - 0;
        W64 wmiss_user = (new_stats->cpurequest.count.miss.write)(user_stats) - 0;
        W64 wmiss_kernel = (new_stats->cpurequest.count.miss.write)(kernel_stats) - 0;
        W64 whit_user = (new_stats->cpurequest.count.hit.write.hit)(user_stats) - 0;
        W64 whit_kernel = (new_stats->cpurequest.count.hit.write.hit)(kernel_stats) - 0;
	W64 whit_forward_user = (new_stats->cpurequest.count.hit.write.forward)(user_stats) - 0;
	W64 whit_forward_kernel = (new_stats->cpurequest.count.hit.write.forward)(kernel_stats) - 0;
        W64 rdepend_user = new_stats->cpurequest.stall.read.dependency(user_stats) - 0;
        W64 rdepend_kernel = new_stats->cpurequest.stall.read.dependency(kernel_stats) - 0;
        W64 rcachep_user = new_stats->cpurequest.stall.read.cache_port(user_stats) - 0;
        W64 rcachep_kernel = new_stats->cpurequest.stall.read.cache_port(kernel_stats) - 0;
        W64 wdepend_user = new_stats->cpurequest.stall.write.dependency(user_stats) - 0;
        W64 wdepend_kernel = new_stats->cpurequest.stall.write.dependency(kernel_stats) - 0;
        W64 wcachep_user = new_stats->cpurequest.stall.write.cache_port(user_stats) - 0;
        W64 wcachep_kernel = new_stats->cpurequest.stall.write.cache_port(kernel_stats) - 0;
        W64 rbufferfull_user = new_stats->cpurequest.stall.read.buffer_full(user_stats) - 0;
        W64 rbufferfull_kernel = new_stats->cpurequest.stall.read.buffer_full(kernel_stats) - 0;
        W64 wbufferfull_user = new_stats->cpurequest.stall.write.buffer_full(user_stats) - 0;
        W64 wbufferfull_kernel = new_stats->cpurequest.stall.write.buffer_full(kernel_stats) - 0;

        if ( (strstr(get_name(), "L1_I") !=NULL) || ( strstr(get_name(), "L1_I_A") !=NULL)) {
                core->icache.read_accesses = (rmiss_user + rmiss_kernel + rhit_user + rhit_kernel + rhit_forward_user +
						rhit_forward_kernel) - (readmiss_user + readmiss_kernel + readhit_user + 
						readhit_kernel + readhit_forward_user + readhit_forward_kernel);
		core->icache.read_misses = (rmiss_user + rmiss_kernel) - (readmiss_user + readmiss_kernel);
		core->icache.conflicts = (rdepend_user + rdepend_kernel + rcachep_user + rcachep_kernel + 
				wdepend_user + wdepend_kernel + wcachep_user + wcachep_kernel +
				rbufferfull_user + rbufferfull_kernel + wbufferfull_user + wbufferfull_kernel)
				- (stall_rdepend_user + stall_rdepend_kernel + stall_rcachep_user + stall_rcachep_kernel + 
				stall_wdepend_user + stall_wdepend_kernel + stall_wcachep_user + stall_wcachep_kernel +
				stall_rbufferfull_user + stall_rbufferfull_kernel + stall_wbufferfull_user + stall_wbufferfull_kernel);
	}
        if ( (strstr(get_name(), "L1_D") !=NULL) || ( strstr(get_name(), "L1_D_A") !=NULL)) {
                core->dcache.read_accesses = (rmiss_user + rmiss_kernel + rhit_user + rhit_kernel + rhit_forward_user +
						rhit_forward_kernel) - (readmiss_user + readmiss_kernel + readhit_user +
						readhit_kernel + readhit_forward_user + readhit_forward_kernel);
		core->dcache.read_misses = (rmiss_user + rmiss_kernel) - (readmiss_user + readmiss_kernel);
                core->dcache.write_accesses = (wmiss_user + wmiss_kernel + whit_user + whit_kernel + whit_forward_user +
						whit_forward_kernel) - (writemiss_user + writemiss_kernel + writehit_user +
						writehit_kernel + writehit_forward_user + writehit_forward_kernel);
		core->dcache.write_misses = (wmiss_user + wmiss_kernel) - (writemiss_user + writemiss_kernel);
		core->dcache.conflicts = (rdepend_user + rdepend_kernel + rcachep_user + rcachep_kernel + 
				wdepend_user + wdepend_kernel + wcachep_user + wcachep_kernel +
				rbufferfull_user + rbufferfull_kernel + wbufferfull_user + wbufferfull_kernel)
				- (stall_rdepend_user + stall_rdepend_kernel + stall_rcachep_user + stall_rcachep_kernel + 
				stall_wdepend_user + stall_wdepend_kernel + stall_wcachep_user + stall_wcachep_kernel +
				stall_rbufferfull_user + stall_rbufferfull_kernel + stall_wbufferfull_user + stall_wbufferfull_kernel);
        }
	if ( strstr(get_name(), "L2") !=NULL) {
                W32 count = coreid;
                mcpat->L2[count].read_accesses = (rmiss_user + rmiss_kernel + rhit_user + rhit_kernel + rhit_forward_user +
						rhit_forward_kernel) - (readmiss_user + readmiss_kernel + readhit_user +
						 readhit_kernel + readhit_forward_user + readhit_forward_kernel);
		mcpat->L2[count].read_misses = (rmiss_user + rmiss_kernel) - (readmiss_user + readmiss_kernel);
                mcpat->L2[count].write_accesses = (wmiss_user + wmiss_kernel + whit_user + whit_kernel + whit_forward_user +
						whit_forward_kernel) - (writemiss_user + writemiss_kernel + writehit_user +
						writehit_kernel + writehit_forward_user + writehit_forward_kernel);
		mcpat->L2[count].write_misses = (wmiss_user + wmiss_kernel) - (writemiss_user + writemiss_kernel);

		mcpat->L2[count].conflicts = (rdepend_user + rdepend_kernel + rcachep_user + rcachep_kernel + 
				wdepend_user + wdepend_kernel + wcachep_user + wcachep_kernel +
				rbufferfull_user + rbufferfull_kernel + wbufferfull_user + wbufferfull_kernel)
				- (stall_rdepend_user + stall_rdepend_kernel + stall_rcachep_user + stall_rcachep_kernel + 
				stall_wdepend_user + stall_wdepend_kernel + stall_wcachep_user + stall_wcachep_kernel +
				stall_rbufferfull_user + stall_rbufferfull_kernel + stall_wbufferfull_user + stall_wbufferfull_kernel);
        }
	if ( strstr(get_name(), "L3") !=NULL) {
                W32 count = coreid;
                mcpat->L3[count].read_accesses = (rmiss_user + rmiss_kernel + rhit_user + rhit_kernel + rhit_forward_user +
						rhit_forward_kernel) - (readmiss_user + readmiss_kernel + readhit_user +
						 readhit_kernel + readhit_forward_user + readhit_forward_kernel);
		mcpat->L3[count].read_misses = (rmiss_user + rmiss_kernel) - (readmiss_user + readmiss_kernel);
                mcpat->L3[count].write_accesses = (wmiss_user + wmiss_kernel + whit_user + whit_kernel + whit_forward_user +
						whit_forward_kernel) - (writemiss_user + writemiss_kernel + writehit_user +
						writehit_kernel + writehit_forward_user + writehit_forward_kernel);
                mcpat->L3[count].write_accesses = (wmiss_user + wmiss_kernel + whit_user + whit_kernel) - (writemiss_user + 
						writemiss_kernel + writehit_user + writehit_kernel);
		mcpat->L3[count].write_misses = (wmiss_user + wmiss_kernel) - (writemiss_user + writemiss_kernel);

		mcpat->L3[count].conflicts = (rdepend_user + rdepend_kernel + rcachep_user + rcachep_kernel + 
				wdepend_user + wdepend_kernel + wcachep_user + wcachep_kernel +
				rbufferfull_user + rbufferfull_kernel + wbufferfull_user + wbufferfull_kernel)
				- (stall_rdepend_user + stall_rdepend_kernel + stall_rcachep_user + stall_rcachep_kernel + 
				stall_wdepend_user + stall_wdepend_kernel + stall_wcachep_user + stall_wcachep_kernel +
				stall_rbufferfull_user + stall_rbufferfull_kernel + stall_wbufferfull_user + stall_wbufferfull_kernel);
	}
        readmiss_user = rmiss_user;
	readmiss_kernel = rmiss_kernel;
	readhit_user = rhit_user;
	readhit_kernel = rhit_kernel;
	readhit_forward_user = rhit_forward_user;
	readhit_forward_kernel = rhit_forward_kernel;
        writemiss_user = wmiss_user;
	writemiss_kernel = wmiss_kernel;
	writehit_user = whit_user;
	writehit_kernel = whit_kernel;
	writehit_forward_user = whit_forward_user;
	writehit_forward_kernel = whit_forward_kernel;
	stall_rdepend_user = rdepend_user;
	stall_rdepend_kernel = rdepend_kernel;
	stall_rcachep_user = rcachep_user;
	stall_rcachep_kernel = rcachep_kernel;
	stall_wdepend_user = wdepend_user;
	stall_wdepend_kernel = wdepend_kernel;
	stall_wcachep_user = wcachep_user;
	stall_wcachep_kernel = wcachep_kernel;
	stall_rbufferfull_user = rbufferfull_user;
	stall_rbufferfull_kernel = rbufferfull_kernel;
	stall_wbufferfull_user = wbufferfull_user;
	stall_wbufferfull_kernel = wbufferfull_kernel;
#if 0
        system_core *core = &(mcpat->core[idx]);
	W64 readacc = 0, readmiss = 0, cflicts = 0;
	W64 writeacc = 0, writemiss = 0;

        if ( (strstr(get_name(), "L1_I") !=NULL) || (strstr(get_name(), "L1_I_A") !=NULL)) {
               	readacc = (new_stats->cpurequest.count.miss.read)(user_stats) +
                          (new_stats->cpurequest.count.hit.read.hit)(user_stats) +
                          (new_stats->cpurequest.count.hit.read.forward)(user_stats);
                readacc = (new_stats->cpurequest.count.miss.read)(kernel_stats) +
                          ((new_stats->cpurequest.count.hit.read.hit)(kernel_stats) +
                          (new_stats->cpurequest.count.hit.read.forward)(kernel_stats)) + readacc;
                core->icache.read_accesses = readacc - readaccesses;
			readaccesses = readacc;

		readmiss = (new_stats->cpurequest.count.miss.read)(user_stats) +
			   (new_stats->cpurequest.count.miss.read)(kernel_stats);
		core->icache.read_misses = readmiss - readmisses;
		readmisses = readmiss;

                cflicts = ((new_stats->cpurequest.stall.read.dependency)(user_stats) +
                             (new_stats->cpurequest.stall.read.cache_port)(user_stats)) +
                             ((new_stats->cpurequest.stall.write.dependency)(user_stats) +
                              (new_stats->cpurequest.stall.write.cache_port)(user_stats));
		cflicts = ((new_stats->cpurequest.stall.read.buffer_full)(user_stats) +
                             (new_stats->cpurequest.stall.write.buffer_full)(user_stats)) + cflicts;
                cflicts = ((new_stats->cpurequest.stall.read.dependency)(kernel_stats) +
                             (new_stats->cpurequest.stall.read.cache_port)(kernel_stats)) +
                             ((new_stats->cpurequest.stall.write.dependency)(kernel_stats) +
                             (new_stats->cpurequest.stall.write.cache_port)(kernel_stats)) + cflicts;
		cflicts = ((new_stats->cpurequest.stall.read.buffer_full)(kernel_stats) +
                             (new_stats->cpurequest.stall.write.buffer_full)(kernel_stats)) + cflicts;
                core->icache.conflicts = cflicts - conflicts;
                conflicts = cflicts;
	}
        if ( (strstr(get_name(), "L1_D") !=NULL) || (strstr(get_name(), "L1_D_A") !=NULL)) {
               	readacc = (new_stats->cpurequest.count.miss.read)(user_stats) +
                          (new_stats->cpurequest.count.hit.read.hit)(user_stats) +
                          (new_stats->cpurequest.count.hit.read.forward)(user_stats);
                readacc = (new_stats->cpurequest.count.miss.read)(kernel_stats) +
                          ((new_stats->cpurequest.count.hit.read.hit)(kernel_stats) +
                          (new_stats->cpurequest.count.hit.read.forward)(kernel_stats)) + readacc;
                core->dcache.read_accesses = readacc - readaccesses;
		readaccesses = readacc;

		readmiss = (new_stats->cpurequest.count.miss.read)(user_stats) +
			   (new_stats->cpurequest.count.miss.read)(kernel_stats);
		core->dcache.read_misses = readmiss - readmisses;
		readmisses = readmiss;

                writeacc = (new_stats->cpurequest.count.miss.write)(user_stats) +
			   ((new_stats->cpurequest.count.hit.write.hit)(user_stats) +
                           (new_stats->cpurequest.count.hit.write.forward)(user_stats));
                writeacc = (new_stats->cpurequest.count.miss.write)(kernel_stats) +
			   ((new_stats->cpurequest.count.hit.write.hit)(kernel_stats) +
                           (new_stats->cpurequest.count.hit.write.forward)(kernel_stats)) + writeacc;
		core->dcache.write_accesses = writeacc - writeaccesses;
		writeaccesses = writeacc;

                writemiss = (new_stats->cpurequest.count.miss.write)(user_stats) +
			    (new_stats->cpurequest.count.miss.write)(kernel_stats);
                core->dcache.write_misses = writemiss - writemisses;
		writemisses = writemiss;

                cflicts = ((new_stats->cpurequest.stall.read.dependency)(user_stats) +
                             (new_stats->cpurequest.stall.read.cache_port)(user_stats)) +
                             ((new_stats->cpurequest.stall.write.dependency)(user_stats) +
                              (new_stats->cpurequest.stall.write.cache_port)(user_stats));
		cflicts = ((new_stats->cpurequest.stall.read.buffer_full)(user_stats) +
                             (new_stats->cpurequest.stall.write.buffer_full)(user_stats)) + cflicts;
                cflicts = ((new_stats->cpurequest.stall.read.dependency)(kernel_stats) +
                             (new_stats->cpurequest.stall.read.cache_port)(kernel_stats)) +
                             ((new_stats->cpurequest.stall.write.dependency)(kernel_stats) +
                             (new_stats->cpurequest.stall.write.cache_port)(kernel_stats)) + cflicts;
		cflicts = ((new_stats->cpurequest.stall.read.buffer_full)(kernel_stats) +
                             (new_stats->cpurequest.stall.write.buffer_full)(kernel_stats)) + cflicts;
                core->dcache.conflicts = cflicts - conflicts;
                conflicts = cflicts;
        }
	if ( strstr(get_name(), "L2") !=NULL) {
                W32 count = coreid;
               	readacc = (new_stats->cpurequest.count.miss.read)(user_stats) +
                          (new_stats->cpurequest.count.hit.read.hit)(user_stats) +
                          (new_stats->cpurequest.count.hit.read.forward)(user_stats);
                readacc = (new_stats->cpurequest.count.miss.read)(kernel_stats) +
                          ((new_stats->cpurequest.count.hit.read.hit)(kernel_stats) +
                          (new_stats->cpurequest.count.hit.read.forward)(kernel_stats)) + readacc;
                mcpat->L2[count].read_accesses = readacc - readaccesses;
		readaccesses = readacc;

		readmiss = (new_stats->cpurequest.count.miss.read)(user_stats) +
			   (new_stats->cpurequest.count.miss.read)(kernel_stats);
		mcpat->L2[count].read_misses = readmiss - readmisses;
		readmisses = readmiss;

                writeacc = (new_stats->cpurequest.count.miss.write)(user_stats) +
			   ((new_stats->cpurequest.count.hit.write.hit)(user_stats) +
                           (new_stats->cpurequest.count.hit.write.forward)(user_stats));
                writeacc = (new_stats->cpurequest.count.miss.write)(kernel_stats) +
			   ((new_stats->cpurequest.count.hit.write.hit)(kernel_stats) +
                           (new_stats->cpurequest.count.hit.write.forward)(kernel_stats)) + writeacc;
		mcpat->L2[count].write_accesses = writeacc - writeaccesses;
		writeaccesses = writeacc;

                writemiss = (new_stats->cpurequest.count.miss.write)(user_stats) +
			    (new_stats->cpurequest.count.miss.write)(kernel_stats);
                mcpat->L2[count].write_misses = writemiss - writemisses;
		writemisses = writemiss;

                cflicts = ((new_stats->cpurequest.stall.read.dependency)(user_stats) +
                             (new_stats->cpurequest.stall.read.cache_port)(user_stats)) +
                             ((new_stats->cpurequest.stall.write.dependency)(user_stats) +
                              (new_stats->cpurequest.stall.write.cache_port)(user_stats));
		cflicts = ((new_stats->cpurequest.stall.read.buffer_full)(user_stats) +
                             (new_stats->cpurequest.stall.write.buffer_full)(user_stats)) + cflicts;
                cflicts = ((new_stats->cpurequest.stall.read.dependency)(kernel_stats) +
                             (new_stats->cpurequest.stall.read.cache_port)(kernel_stats)) +
                             ((new_stats->cpurequest.stall.write.dependency)(kernel_stats) +
                             (new_stats->cpurequest.stall.write.cache_port)(kernel_stats)) + cflicts;
		cflicts = ((new_stats->cpurequest.stall.read.buffer_full)(kernel_stats) +
                             (new_stats->cpurequest.stall.write.buffer_full)(kernel_stats)) + cflicts;
                mcpat->L2[count].conflicts = cflicts - conflicts;
                conflicts = cflicts;
        }
	if ( strstr(get_name(), "L3") !=NULL) {
                W32 count = coreid;
               	readacc = (new_stats->cpurequest.count.miss.read)(user_stats) +
                          (new_stats->cpurequest.count.hit.read.hit)(user_stats) +
                          (new_stats->cpurequest.count.hit.read.forward)(user_stats);
                readacc = (new_stats->cpurequest.count.miss.read)(kernel_stats) +
                          ((new_stats->cpurequest.count.hit.read.hit)(kernel_stats) +
                          (new_stats->cpurequest.count.hit.read.forward)(kernel_stats)) + readacc;
                mcpat->L3[count].read_accesses = readacc - readaccesses;
		readaccesses = readacc;

		readmiss = (new_stats->cpurequest.count.miss.read)(user_stats) +
			   (new_stats->cpurequest.count.miss.read)(kernel_stats);
		mcpat->L3[count].read_misses = readmiss - readmisses;
		readmisses = readmiss;

                writeacc = (new_stats->cpurequest.count.miss.write)(user_stats) +
			   ((new_stats->cpurequest.count.hit.write.hit)(user_stats) +
                           (new_stats->cpurequest.count.hit.write.forward)(user_stats));
                writeacc = (new_stats->cpurequest.count.miss.write)(kernel_stats) +
			   ((new_stats->cpurequest.count.hit.write.hit)(kernel_stats) +
                           (new_stats->cpurequest.count.hit.write.forward)(kernel_stats)) + writeacc;
		mcpat->L3[count].write_accesses = writeacc - writeaccesses;
		writeaccesses = writeacc;

                writemiss = (new_stats->cpurequest.count.miss.write)(user_stats) +
			    (new_stats->cpurequest.count.miss.write)(kernel_stats);
                mcpat->L3[count].write_misses = writemiss - writemisses;
		writemisses = writemiss;

                cflicts = ((new_stats->cpurequest.stall.read.dependency)(user_stats) +
                             (new_stats->cpurequest.stall.read.cache_port)(user_stats)) +
                             ((new_stats->cpurequest.stall.write.dependency)(user_stats) +
                              (new_stats->cpurequest.stall.write.cache_port)(user_stats));
		cflicts = ((new_stats->cpurequest.stall.read.buffer_full)(user_stats) +
                             (new_stats->cpurequest.stall.write.buffer_full)(user_stats)) + cflicts;
                cflicts = ((new_stats->cpurequest.stall.read.dependency)(kernel_stats) +
                             (new_stats->cpurequest.stall.read.cache_port)(kernel_stats)) +
                             ((new_stats->cpurequest.stall.write.dependency)(kernel_stats) +
                             (new_stats->cpurequest.stall.write.cache_port)(kernel_stats)) + cflicts;
		cflicts = ((new_stats->cpurequest.stall.read.buffer_full)(kernel_stats) +
                             (new_stats->cpurequest.stall.write.buffer_full)(kernel_stats)) + cflicts;
                mcpat->L3[count].conflicts = cflicts - conflicts;
                conflicts = cflicts;
	}
#endif
}
