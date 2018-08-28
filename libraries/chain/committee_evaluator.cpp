#include <golos/chain/steem_evaluator.hpp>
#include <golos/chain/database.hpp>
#include <golos/chain/steem_objects.hpp>
#include <golos/chain/committee_objects.hpp>

namespace golos { namespace chain {

     void committee_worker_create_request_evaluator::do_apply(const committee_worker_create_request_operation& o) {
        _db.get_account(o.creator);
        _db.get_account(o.worker);

        bool find=false;

        const auto &idx = _db.get_index<committee_request_index>().indices().get<by_id>();
        for(const auto& itr : idx){
            if(itr.creator==o.creator){
                if(std::strcmp(itr.url.c_str(),o.url.c_str())==0){
                    find=true;
                    FC_ASSERT(!find, "Committee request with same creator and url already exist.");
                }
                if(COMMITTEE_REQUEST_PER_TIME>(_db.head_block_time() - itr.start_time).to_seconds()){
                    find=true;
                    FC_ASSERT(!find, "Committee request can not be created by same creator before COMMITTEE_REQUEST_PER_TIME ends.");
                }
            }
        }

        if(!find){
            const dynamic_global_property_object &dgp = _db.get_dynamic_global_properties();
            uint32_t committee_request_id = dgp.committee_requests;
            committee_request_id++;
            _db.create<committee_request_object>([&](committee_request_object& c) {
                c.request_id = committee_request_id;
                c.creator = o.creator;
                c.worker = o.worker;
                c.url.append(o.url.c_str());
                c.required_amount_min = o.required_amount_min;
                c.required_amount_max = o.required_amount_max;
                c.start_time = _db.head_block_time();
                c.duration = o.duration;
                c.end_time = c.start_time + fc::seconds(o.duration);
                c.status = 0;
                c.votes_count = 0;
            });
            _db.modify(dgp, [&](dynamic_global_property_object &dgp) {
                dgp.committee_requests = committee_request_id;
            });
        }
    }

    void committee_worker_cancel_request_evaluator::do_apply(const committee_worker_cancel_request_operation& o) {
        _db.get_account(o.creator);
        const auto &idx = _db.get_index<committee_request_index>().indices().get<by_request_id>();
        auto itr = idx.find(o.request_id);
        FC_ASSERT(itr != idx.end(), "Committee request id not found.");

        if(itr->creator!=o.creator)
            FC_ASSERT(false, "Committee request can canceled only by creator.");

        if(itr->status == 0){
            _db.modify(*itr, [&](committee_request_object& c) {
                c.status = 1;
                c.conclusion_time = _db.head_block_time();
            });
        }
        else{
            FC_ASSERT(false, "Committee request is not active.");
        }
    }

    void committee_vote_request_evaluator::do_apply(const committee_vote_request_operation& o) {
        _db.get_account(o.voter);
        const auto &idx = _db.get_index<committee_request_index>().indices().get<by_request_id>();
        auto itr = idx.find(o.request_id);
        FC_ASSERT(itr != idx.end(), "Committee request id not found.");

        if(itr->status == 0){
            bool find=false;
            const auto &vote_idx = _db.get_index<committee_vote_index>().indices().get<by_request_id>();
            auto vote_itr = vote_idx.lower_bound(itr->request_id);
            while (vote_itr != vote_idx.end() &&
                   vote_itr->request_id == itr->request_id) {
                const auto &cur_vote = *vote_itr;
                ++vote_itr;
                if(cur_vote.voter==o.voter){
                    find = true;
                    FC_ASSERT(cur_vote.vote_percent != o.vote_percent, "Committee vote percent equal last vote.");
                    _db.modify(cur_vote, [&](committee_vote_object &c) {
                        c.vote_percent = o.vote_percent;
                        c.last_update = _db.head_block_time();
                    });
                }
            }
            if(!find){
                _db.create<committee_vote_object>([&](committee_vote_object& c) {
                    c.request_id = itr->request_id;
                    c.voter = o.voter;
                    c.vote_percent = o.vote_percent;
                    c.last_update = _db.head_block_time();
                });
                _db.modify(*itr, [&](committee_request_object& c) {
                    c.votes_count++;
                });
            }
        }
        else{
            FC_ASSERT(false, "Committee request is not active.");
        }
    }

} } // golos::chain