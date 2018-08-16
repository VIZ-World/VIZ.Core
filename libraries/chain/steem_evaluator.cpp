#include <golos/chain/steem_evaluator.hpp>
#include <golos/chain/database.hpp>
#include <golos/chain/custom_operation_interpreter.hpp>
#include <golos/chain/steem_objects.hpp>
#include <golos/chain/block_summary_object.hpp>

namespace golos { namespace chain {
        using fc::uint128_t;

        inline void validate_permlink(const string &permlink) {
            FC_ASSERT(permlink.size() < STEEMIT_MAX_PERMLINK_LENGTH, "permlink is too long");
            FC_ASSERT(fc::is_utf8(permlink), "permlink not formatted in UTF8");
        }

        struct strcmp_equal {
            bool operator()(const shared_string &a, const string &b) {
                return a.size() == b.size() ||
                       std::strcmp(a.c_str(), b.c_str()) == 0;
            }
        };

        void store_account_json_metadata(
            database& db, const account_name_type& account, const string& json_metadata, bool skip_empty = false
        ) {
#ifndef IS_LOW_MEM
            if (skip_empty && json_metadata.size() == 0)
                return;

            const auto& idx = db.get_index<account_metadata_index>().indices().get<by_account>();
            auto itr = idx.find(account);
            if (itr != idx.end()) {
                db.modify(*itr, [&](account_metadata_object& a) {
                    from_string(a.json_metadata, json_metadata);
                });
            } else {
                // Note: this branch should be executed only on account creation.
                db.create<account_metadata_object>([&](account_metadata_object& a) {
                    a.account = account;
                    from_string(a.json_metadata, json_metadata);
                });
            }
#endif
        }

        void account_create_evaluator::do_apply(const account_create_operation& o) {
            const auto& creator = _db.get_account(o.creator);
            FC_ASSERT(creator.balance >= o.fee, "Insufficient balance to create account.",
                ("creator.balance", creator.balance)("required", o.fee));
            FC_ASSERT(creator.available_vesting_shares(true) >= o.delegation,
                "Insufficient vesting shares to delegate to new account.",
                ("creator.vesting_shares", creator.vesting_shares)
                ("creator.delegated_vesting_shares", creator.delegated_vesting_shares)
                ("required", o.delegation));

            const auto& v_share_price = _db.get_dynamic_global_properties().get_vesting_share_price();
            const auto& median_props = _db.get_witness_schedule_object().median_props;
            auto target_delegation =
                median_props.create_account_delegation_ratio *
                median_props.account_creation_fee * v_share_price;
            auto current_delegation =
                median_props.create_account_delegation_ratio * o.fee * v_share_price + o.delegation;

            FC_ASSERT(current_delegation >= target_delegation,
                "Inssufficient Delegation ${f} required, ${p} provided.",
                ("f", target_delegation)("p", current_delegation)("o.fee", o.fee) ("o.delegation", o.delegation));

            for (auto& a : o.owner.account_auths) {
                _db.get_account(a.first);
            }
            for (auto& a : o.active.account_auths) {
                _db.get_account(a.first);
            }
            for (auto& a : o.posting.account_auths) {
                _db.get_account(a.first);
            }

            const auto now = _db.head_block_time();

            _db.modify(creator, [&](account_object& c) {
                c.balance -= o.fee;
                c.delegated_vesting_shares += o.delegation;
            });
            const auto& new_account = _db.create<account_object>([&](account_object& acc) {
                acc.name = o.new_account_name;
                acc.memo_key = o.memo_key;
                acc.created = now;
                acc.last_vote_time = now;
                acc.recovery_account = o.creator;
                acc.received_vesting_shares = o.delegation;
                if(o.referrer.size()){
                	const auto &referrer = _db.get_account(o.referrer);
		            acc.referrer = referrer.name;
	            }
            });
            store_account_json_metadata(_db, o.new_account_name, o.json_metadata);

            _db.create<account_authority_object>([&](account_authority_object& auth) {
                auth.account = o.new_account_name;
                auth.owner = o.owner;
                auth.active = o.active;
                auth.posting = o.posting;
                auth.last_owner_update = fc::time_point_sec::min();
            });
            if (o.delegation.amount > 0) {  // Is it needed to allow zero delegation in this method ?
                _db.create<vesting_delegation_object>([&](vesting_delegation_object& d) {
                    d.delegator = o.creator;
                    d.delegatee = o.new_account_name;
                    d.vesting_shares = o.delegation;
                    d.min_delegation_time = now + fc::seconds(median_props.create_account_delegation_time);
                });
            }
            if (o.fee.amount > 0) {
                _db.create_vesting(new_account, o.fee);
            }
        }

        void account_update_evaluator::do_apply(const account_update_operation &o) {
            database &_db = db();
            FC_ASSERT(o.account != STEEMIT_TEMP_ACCOUNT, "Cannot update temp account.");

            if (o.posting) {
                     o.posting->validate();
            }

            const auto &account = _db.get_account(o.account);
            const auto &account_auth = _db.get<account_authority_object, by_account>(o.account);

            if (o.owner) {
                FC_ASSERT(_db.head_block_time() -
                          account_auth.last_owner_update >
                          STEEMIT_OWNER_UPDATE_LIMIT, "Owner authority can only be updated once an hour.");
                for (auto a: o.owner->account_auths) {
                    _db.get_account(a.first);
                }

                _db.update_owner_authority(account, *o.owner);
            }

            if (o.active)
            {
                for (auto a: o.active->account_auths) {
                    _db.get_account(a.first);
                }
            }

            if (o.posting)
            {
                for (auto a: o.posting->account_auths) {
                    _db.get_account(a.first);
                }
            }

            _db.modify(account, [&](account_object &acc) {
                if (o.memo_key != public_key_type()) {
                    acc.memo_key = o.memo_key;
                }
                acc.last_account_update = _db.head_block_time();
            });
            store_account_json_metadata(_db, account.name, o.json_metadata, true);

            if (o.active || o.posting) {
                _db.modify(account_auth, [&](account_authority_object &auth) {
                    if (o.active) {
                        auth.active = *o.active;
                    }
                    if (o.posting) {
                        auth.posting = *o.posting;
                    }
                });
            }

        }

        void account_metadata_evaluator::do_apply(const account_metadata_operation& o) {
            const auto& account = _db.get_account(o.account);
            _db.modify(account, [&](account_object& a) {
                a.last_account_update = _db.head_block_time();
            });
            store_account_json_metadata(_db, o.account, o.json_metadata);
        }

/**
 *  Because net_rshares is 0 there is no need to update any pending payout calculations or parent posts.
 */
        void delete_comment_evaluator::do_apply(const delete_comment_operation &o) {
            database &_db = db();
            const auto &comment = _db.get_comment(o.author, o.permlink);
            FC_ASSERT(comment.children ==
                      0, "Cannot delete a comment with replies.");

            if (_db.is_producing()) {
                FC_ASSERT(comment.net_rshares <=
                          0, "Cannot delete a comment with network positive votes.");
            }
            if (comment.net_rshares > 0) {
                return;
            }

            const auto &vote_idx = _db.get_index<comment_vote_index>().indices().get<by_comment_voter>();

            auto vote_itr = vote_idx.lower_bound(comment_id_type(comment.id));
            while (vote_itr != vote_idx.end() &&
                   vote_itr->comment == comment.id) {
                const auto &cur_vote = *vote_itr;
                ++vote_itr;
                _db.remove(cur_vote);
            }

            const auto &auth = _db.get_account(comment.author); /// prove it exists
            db().modify(auth, [&](account_object &a) {
                if( comment.parent_author == STEEMIT_ROOT_POST_PARENT ) {
                    a.post_count--;
                }
                else{
                    a.comment_count--;
                }
            });

            /// this loop can be skiped for validate-only nodes as it is merely gathering stats for indicies
            if (comment.parent_author != STEEMIT_ROOT_POST_PARENT) {
                auto parent = &_db.get_comment(comment.parent_author, comment.parent_permlink);
                auto now = _db.head_block_time();
                while (parent) {
                    _db.modify(*parent, [&](comment_object &p) {
                        p.children--;
                        p.active = now;
                    });
#ifndef IS_LOW_MEM
                    if (parent->parent_author != STEEMIT_ROOT_POST_PARENT) {
                        parent = &_db.get_comment(parent->parent_author, parent->parent_permlink);
                    } else
#endif
                    {
                        parent = nullptr;
                    }
                }
            }
#ifndef IS_LOW_MEM
            auto& content = _db.get_comment_content(comment.id);
            _db.remove(content);
#endif
            _db.remove(comment);
        }

        struct comment_extension_visitor {
            comment_extension_visitor(const comment_object &c, database &db)
                    : _c(c), _db(db) {
            }

            using result_type = void;

            const comment_object &_c;
            database &_db;

            void operator()(const comment_payout_beneficiaries &cpb) const {
                if (_db.is_producing()) {
                    FC_ASSERT(cpb.beneficiaries.size() <= STEEMIT_MAX_COMMENT_BENEFICIARIES,
                              "Cannot specify more than ${m} beneficiaries.", ("m", STEEMIT_MAX_COMMENT_BENEFICIARIES));
                }

                FC_ASSERT(_c.beneficiaries.size() == 0, "Comment already has beneficiaries specified.");
                FC_ASSERT(_c.abs_rshares == 0, "Comment must not have been voted on before specifying beneficiaries.");

                _db.modify(_c, [&](comment_object &c) {
                    for (auto &b : cpb.beneficiaries) {
                        auto acc = _db.find< account_object, by_name >( b.account );
                        FC_ASSERT( acc != nullptr, "Beneficiary \"${a}\" must exist.", ("a", b.account) );
                        c.beneficiaries.push_back(b);
                    }
                });
            }
        };

        void comment_evaluator::do_apply(const comment_operation &o) {
            try {
                database &_db = db();

                FC_ASSERT(o.title.size() + o.body.size() +
                          o.json_metadata.size(), "Cannot update comment because nothing appears to be changing.");

                const auto &by_permlink_idx = _db.get_index<comment_index>().indices().get<by_permlink>();
                auto itr = by_permlink_idx.find(boost::make_tuple(o.author, o.permlink));

                const auto &auth = _db.get_account(o.author); /// prove it exists

                comment_id_type id;

                const comment_object *parent = nullptr;
                if (o.parent_author != STEEMIT_ROOT_POST_PARENT) {
                    parent = &_db.get_comment(o.parent_author, o.parent_permlink);
                    auto max_depth = STEEMIT_MAX_COMMENT_DEPTH;
                    FC_ASSERT(parent->depth < max_depth,
                              "Comment is nested ${x} posts deep, maximum depth is ${y}.",
                              ("x", parent->depth)("y", max_depth));
                }
                auto now = _db.head_block_time();

                if (itr == by_permlink_idx.end()) {
                    if (o.parent_author == STEEMIT_ROOT_POST_PARENT)
                        FC_ASSERT((now - auth.last_root_post) >
                                  STEEMIT_MIN_ROOT_COMMENT_INTERVAL, "You may only post once every 1 second.", ("now", now)("last_root_post", auth.last_root_post));
                    else
                        FC_ASSERT((now - auth.last_post) >
                                  STEEMIT_MIN_REPLY_INTERVAL, "You may only comment once every 1 second.", ("now", now)("auth.last_post", auth.last_post));

                    db().modify(auth, [&](account_object &a) {
                        a.last_post = now;
                        if( o.parent_author == STEEMIT_ROOT_POST_PARENT ) {
                            a.last_root_post = now;
                            a.post_count++;
                        }
                        else{
                            a.comment_count++;
                        }
                    });

                    const auto &new_comment = _db.create<comment_object>([&](comment_object &com) {
                        validate_permlink(o.parent_permlink);
                        validate_permlink(o.permlink);

                        com.author = o.author;
                        from_string(com.permlink, o.permlink);
                        com.last_update = _db.head_block_time();
                        com.created = com.last_update;
                        com.active = com.last_update;
                        com.last_payout = fc::time_point_sec::min();
                        com.cashout_time = com.created + STEEMIT_CASHOUT_WINDOW_SECONDS;
                        com.curation_percent = o.curation_percent;

                        if (o.parent_author == STEEMIT_ROOT_POST_PARENT) {
                            com.parent_author = "";
                            from_string(com.parent_permlink, o.parent_permlink);
                            com.root_comment = com.id;
                        }
                        else {
                            com.parent_author = parent->author;
                            com.parent_permlink = parent->permlink;
                            com.depth = parent->depth + 1;
                            com.root_comment = parent->root_comment;
                        }
                    });

                    for (auto &e : o.extensions) {
                        e.visit(comment_extension_visitor(new_comment, _db));
                    }

                    id = new_comment.id;
#ifndef IS_LOW_MEM
                    _db.create<comment_content_object>([&](comment_content_object& con) {
                        con.comment = id;
                        from_string(con.title, o.title);
                        if (o.body.size() < 1024*1024*128) {
                            from_string(con.body, o.body);
                        }
                        if (fc::is_utf8(o.json_metadata)) {
                            from_string(con.json_metadata, o.json_metadata);
                        } else {
                            wlog("Comment ${a}/${p} contains invalid UTF-8 metadata",
                                 ("a", o.author)("p", o.permlink));
                        }
                    });
#endif
/// this loop can be skiped for validate-only nodes as it is merely gathering stats for indicies
                    auto now = _db.head_block_time();
                    while (parent) {
                        _db.modify(*parent, [&](comment_object &p) {
                            p.children++;
                            p.active = now;
                        });
#ifndef IS_LOW_MEM
                        if (parent->parent_author != STEEMIT_ROOT_POST_PARENT) {
                            parent = &_db.get_comment(parent->parent_author, parent->parent_permlink);
                        } else
#endif
                        {
                            parent = nullptr;
                        }
                    }

                } else // start edit case
                {
                    const auto &comment = *itr;
                    _db.modify(comment, [&](comment_object &com) {
                        com.last_update = _db.head_block_time();
                        com.active = com.last_update;
                        strcmp_equal equal;

                        if (!parent) {
                            FC_ASSERT(com.parent_author ==
                                      account_name_type(), "The parent of a comment cannot change.");
                            FC_ASSERT(equal(com.parent_permlink, o.parent_permlink), "The permlink of a comment cannot change.");
                        } else {
                            FC_ASSERT(com.parent_author ==
                                      o.parent_author, "The parent of a comment cannot change.");
                            FC_ASSERT(equal(com.parent_permlink, o.parent_permlink), "The permlink of a comment cannot change.");
                        }
                    });

                    for (auto &e : o.extensions) {
                        e.visit(comment_extension_visitor(comment, _db));
                    }

#ifndef IS_LOW_MEM
                    _db.modify(_db.get< comment_content_object, by_comment >( comment.id ), [&]( comment_content_object& con ) {
                        if (o.title.size())
                            from_string(con.title, o.title);
                        if (o.json_metadata.size())
                            from_string(con.json_metadata, o.json_metadata );
                        if (o.body.size())
                            from_string(con.body, o.body);
                    });
#endif

                } // end EDIT case

            } FC_CAPTURE_AND_RETHROW((o))
        }

        void escrow_transfer_evaluator::do_apply(const escrow_transfer_operation &o) {
            try {
                database &_db = db();

                const auto &from_account = _db.get_account(o.from);
                _db.get_account(o.to);
                _db.get_account(o.agent);

                FC_ASSERT(o.ratification_deadline >
                          _db.head_block_time(), "The escorw ratification deadline must be after head block time.");
                FC_ASSERT(o.escrow_expiration >
                          _db.head_block_time(), "The escrow expiration must be after head block time.");

                asset steem_spent = o.steem_amount;
                if (o.fee.symbol == STEEM_SYMBOL) {
                    steem_spent += o.fee;
                }

                FC_ASSERT(from_account.balance >=
                          steem_spent, "Account cannot cover STEEM costs of escrow. Required: ${r} Available: ${a}", ("r", steem_spent)("a", from_account.balance));

                _db.adjust_balance(from_account, -steem_spent);

                _db.create<escrow_object>([&](escrow_object &esc) {
                    esc.escrow_id = o.escrow_id;
                    esc.from = o.from;
                    esc.to = o.to;
                    esc.agent = o.agent;
                    esc.ratification_deadline = o.ratification_deadline;
                    esc.escrow_expiration = o.escrow_expiration;
                    esc.steem_balance = o.steem_amount;
                    esc.pending_fee = o.fee;
                });
            }
            FC_CAPTURE_AND_RETHROW((o))
        }

        void escrow_approve_evaluator::do_apply(const escrow_approve_operation &o) {
            try {
                database &_db = db();
                const auto &escrow = _db.get_escrow(o.from, o.escrow_id);

                FC_ASSERT(escrow.to ==
                          o.to, "Operation 'to' (${o}) does not match escrow 'to' (${e}).", ("o", o.to)("e", escrow.to));
                FC_ASSERT(escrow.agent ==
                          o.agent, "Operation 'agent' (${a}) does not match escrow 'agent' (${e}).", ("o", o.agent)("e", escrow.agent));
                FC_ASSERT(escrow.ratification_deadline >=
                          _db.head_block_time(), "The escrow ratification deadline has passed. Escrow can no longer be ratified.");

                bool reject_escrow = !o.approve;

                if (o.who == o.to) {
                    FC_ASSERT(!escrow.to_approved, "Account 'to' (${t}) has already approved the escrow.", ("t", o.to));

                    if (!reject_escrow) {
                        _db.modify(escrow, [&](escrow_object &esc) {
                            esc.to_approved = true;
                        });
                    }
                }
                if (o.who == o.agent) {
                    FC_ASSERT(!escrow.agent_approved, "Account 'agent' (${a}) has already approved the escrow.", ("a", o.agent));

                    if (!reject_escrow) {
                        _db.modify(escrow, [&](escrow_object &esc) {
                            esc.agent_approved = true;
                        });
                    }
                }

                if (reject_escrow) {
                    const auto &from_account = _db.get_account(o.from);
                    _db.adjust_balance(from_account, escrow.steem_balance);
                    _db.adjust_balance(from_account, escrow.pending_fee);

                    _db.remove(escrow);
                } else if (escrow.to_approved && escrow.agent_approved) {
                    const auto &agent_account = _db.get_account(o.agent);
                    _db.adjust_balance(agent_account, escrow.pending_fee);

                    _db.modify(escrow, [&](escrow_object &esc) {
                        esc.pending_fee.amount = 0;
                    });
                }
            }
            FC_CAPTURE_AND_RETHROW((o))
        }

        void escrow_dispute_evaluator::do_apply(const escrow_dispute_operation &o) {
            try {
                database &_db = db();
                _db.get_account(o.from); // Verify from account exists

                const auto &e = _db.get_escrow(o.from, o.escrow_id);
                FC_ASSERT(_db.head_block_time() <
                          e.escrow_expiration, "Disputing the escrow must happen before expiration.");
                FC_ASSERT(e.to_approved &&
                          e.agent_approved, "The escrow must be approved by all parties before a dispute can be raised.");
                FC_ASSERT(!e.disputed, "The escrow is already under dispute.");
                FC_ASSERT(e.to ==
                          o.to, "Operation 'to' (${o}) does not match escrow 'to' (${e}).", ("o", o.to)("e", e.to));
                FC_ASSERT(e.agent ==
                          o.agent, "Operation 'agent' (${a}) does not match escrow 'agent' (${e}).", ("o", o.agent)("e", e.agent));

                _db.modify(e, [&](escrow_object &esc) {
                    esc.disputed = true;
                });
            }
            FC_CAPTURE_AND_RETHROW((o))
        }

        void escrow_release_evaluator::do_apply(const escrow_release_operation &o) {
            try {
                database &_db = db();
                _db.get_account(o.from); // Verify from account exists
                const auto &receiver_account = _db.get_account(o.receiver);

                const auto &e = _db.get_escrow(o.from, o.escrow_id);
                FC_ASSERT(e.steem_balance >=
                          o.steem_amount, "Release amount exceeds escrow balance. Amount: ${a}, Balance: ${b}", ("a", o.steem_amount)("b", e.steem_balance));
                FC_ASSERT(e.to ==
                          o.to, "Operation 'to' (${o}) does not match escrow 'to' (${e}).", ("o", o.to)("e", e.to));
                FC_ASSERT(e.agent ==
                          o.agent, "Operation 'agent' (${a}) does not match escrow 'agent' (${e}).", ("o", o.agent)("e", e.agent));
                FC_ASSERT(o.receiver == e.from || o.receiver ==
                                                  e.to, "Funds must be released to 'from' (${f}) or 'to' (${t})", ("f", e.from)("t", e.to));
                FC_ASSERT(e.to_approved &&
                          e.agent_approved, "Funds cannot be released prior to escrow approval.");

                // If there is a dispute regardless of expiration, the agent can release funds to either party
                if (e.disputed) {
                    FC_ASSERT(o.who ==
                              e.agent, "Only 'agent' (${a}) can release funds in a disputed escrow.", ("a", e.agent));
                } else {
                    FC_ASSERT(o.who == e.from || o.who ==
                                                 e.to, "Only 'from' (${f}) and 'to' (${t}) can release funds from a non-disputed escrow", ("f", e.from)("t", e.to));

                    if (e.escrow_expiration > _db.head_block_time()) {
                        // If there is no dispute and escrow has not expired, either party can release funds to the other.
                        if (o.who == e.from) {
                            FC_ASSERT(o.receiver ==
                                      e.to, "Only 'from' (${f}) can release funds to 'to' (${t}).", ("f", e.from)("t", e.to));
                        } else if (o.who == e.to) {
                            FC_ASSERT(o.receiver ==
                                      e.from, "Only 'to' (${t}) can release funds to 'from' (${t}).", ("f", e.from)("t", e.to));
                        }
                    }
                }
                // If escrow expires and there is no dispute, either party can release funds to either party.

                _db.adjust_balance(receiver_account, o.steem_amount);

                _db.modify(e, [&](escrow_object &esc) {
                    esc.steem_balance -= o.steem_amount;
                });

                if (e.steem_balance.amount == 0) {
                    _db.remove(e);
                }
            }
            FC_CAPTURE_AND_RETHROW((o))
        }

        void transfer_evaluator::do_apply(const transfer_operation &o) {
            database &_db = db();
            const auto &from_account = _db.get_account(o.from);
            const auto &to_account = _db.get_account(o.to);

            FC_ASSERT(_db.get_balance(from_account, o.amount.symbol) >=
                      o.amount, "Account does not have sufficient funds for transfer.");
            _db.adjust_balance(from_account, -o.amount);
            _db.adjust_balance(to_account, o.amount);

            //VIZ support anonymous account creation
            if(STEEMIT_ANONYMOUS_ACCOUNT==to_account.name){
            	const auto& median_props = _db.get_witness_schedule_object().median_props;
            	FC_ASSERT(o.amount >= median_props.account_creation_fee,
            	    "Inssufficient amount ${f} required, ${p} provided.",
            	    ("f", median_props.account_creation_fee)("p", o.amount));
            	if(o.memo.size()){
            		public_key_type key_from_memo(o.memo);
            		const auto& meta = _db.get<account_metadata_object, by_account>(to_account.name);
            		int anonymous_num=std::stoi(meta.json_metadata.c_str());
            		anonymous_num++;
            		store_account_json_metadata(_db, STEEMIT_ANONYMOUS_ACCOUNT,fc::to_string(anonymous_num));
            		const auto now = _db.head_block_time();
            		account_name_type new_account_name="n" + fc::to_string(anonymous_num) + "." + STEEMIT_ANONYMOUS_ACCOUNT;

                    _db.create<account_object>([&](account_object &acc) {
                        acc.name = new_account_name;
                        acc.memo_key = key_from_memo;
                        acc.created = _db.head_block_time();
                        acc.recovery_account = "";
                        acc.created = now;
                    });
                    _db.create<account_authority_object>([&](account_authority_object &auth) {
                    	auth.account = new_account_name;
                    	auth.owner.add_authority(key_from_memo, 1);
                    	auth.owner.weight_threshold = 1;
                    	auth.active = auth.owner;
                    	auth.posting = auth.active;
                    });
                    _db.create<account_metadata_object>([&](account_metadata_object& m) {
                        m.account = new_account_name;
                    });
                    const auto &new_account = _db.get_account(new_account_name);
                    _db.adjust_balance(to_account, -o.amount);
                    _db.create_vesting(new_account, o.amount);
	            }
            }
        }

        void transfer_to_vesting_evaluator::do_apply(const transfer_to_vesting_operation &o) {
            database &_db = db();

            const auto &from_account = _db.get_account(o.from);
            const auto &to_account = o.to.size() ? _db.get_account(o.to)
                                                 : from_account;

            FC_ASSERT(_db.get_balance(from_account, STEEM_SYMBOL) >=
                      o.amount, "Account does not have sufficient GOLOS for transfer.");
            _db.adjust_balance(from_account, -o.amount);
            _db.create_vesting(to_account, o.amount);
        }

        void withdraw_vesting_evaluator::do_apply(const withdraw_vesting_operation &o) {
            database &_db = db();

            const auto &account = _db.get_account(o.account);

            FC_ASSERT(account.vesting_shares.amount >= 0,
                "Account does not have sufficient Golos Power for withdraw.");
            FC_ASSERT(account.available_vesting_shares() >= o.vesting_shares,
                "Account does not have sufficient Golos Power for withdraw.");
            FC_ASSERT(o.vesting_shares.amount >= 0, "Cannot withdraw negative VESTS.");

            if (o.vesting_shares.amount == 0) {
                FC_ASSERT(account.vesting_withdraw_rate.amount !=
                          0, "This operation would not change the vesting withdraw rate.");

                _db.modify(account, [&](account_object &a) {
                    a.vesting_withdraw_rate = asset(0, VESTS_SYMBOL);
                    a.next_vesting_withdrawal = time_point_sec::maximum();
                    a.to_withdraw = 0;
                    a.withdrawn = 0;
                });
            }
            else {
                int vesting_withdraw_intervals = STEEMIT_VESTING_WITHDRAW_INTERVALS;

                _db.modify(account, [&](account_object &a) {
                    auto new_vesting_withdraw_rate = asset(
                            o.vesting_shares.amount /
                            vesting_withdraw_intervals, VESTS_SYMBOL);

                    if (new_vesting_withdraw_rate.amount == 0)
                        new_vesting_withdraw_rate.amount = 1;

                    FC_ASSERT(account.vesting_withdraw_rate !=
                              new_vesting_withdraw_rate, "This operation would not change the vesting withdraw rate.");

                    a.vesting_withdraw_rate = new_vesting_withdraw_rate;
                    a.next_vesting_withdrawal = _db.head_block_time() +
                                                fc::seconds(STEEMIT_VESTING_WITHDRAW_INTERVAL_SECONDS);
                    a.to_withdraw = o.vesting_shares.amount;
                    a.withdrawn = 0;
                });
            }
        }

        void set_withdraw_vesting_route_evaluator::do_apply(const set_withdraw_vesting_route_operation &o) {
            try {
                database &_db = db();
                const auto &from_account = _db.get_account(o.from_account);
                const auto &to_account = _db.get_account(o.to_account);
                const auto &wd_idx = _db.get_index<withdraw_vesting_route_index>().indices().get<by_withdraw_route>();
                auto itr = wd_idx.find(boost::make_tuple(from_account.id, to_account.id));

                if (itr == wd_idx.end()) {
                    FC_ASSERT(
                            o.percent != 0, "Cannot create a 0% destination.");
                    FC_ASSERT(from_account.withdraw_routes <
                              STEEMIT_MAX_WITHDRAW_ROUTES, "Account already has the maximum number of routes.");

                    _db.create<withdraw_vesting_route_object>([&](withdraw_vesting_route_object &wvdo) {
                        wvdo.from_account = from_account.id;
                        wvdo.to_account = to_account.id;
                        wvdo.percent = o.percent;
                        wvdo.auto_vest = o.auto_vest;
                    });

                    _db.modify(from_account, [&](account_object &a) {
                        a.withdraw_routes++;
                    });
                } else if (o.percent == 0) {
                    _db.remove(*itr);

                    _db.modify(from_account, [&](account_object &a) {
                        a.withdraw_routes--;
                    });
                } else {
                    _db.modify(*itr, [&](withdraw_vesting_route_object &wvdo) {
                        wvdo.from_account = from_account.id;
                        wvdo.to_account = to_account.id;
                        wvdo.percent = o.percent;
                        wvdo.auto_vest = o.auto_vest;
                    });
                }

                itr = wd_idx.upper_bound(boost::make_tuple(from_account.id, account_id_type()));
                uint16_t total_percent = 0;

                while (itr->from_account == from_account.id &&
                       itr != wd_idx.end()) {
                    total_percent += itr->percent;
                    ++itr;
                }

                FC_ASSERT(total_percent <=
                          STEEMIT_100_PERCENT, "More than 100% of vesting withdrawals allocated to destinations.");
            }
            FC_CAPTURE_AND_RETHROW()
        }

        void account_witness_proxy_evaluator::do_apply(const account_witness_proxy_operation &o) {
            database &_db = db();
            const auto &account = _db.get_account(o.account);
            FC_ASSERT(account.proxy != o.proxy, "Proxy must change.");

            /// remove all current votes
            std::array<share_type, STEEMIT_MAX_PROXY_RECURSION_DEPTH + 1> delta;
            delta[0] = -account.vesting_shares.amount;
            for (int i = 0; i < STEEMIT_MAX_PROXY_RECURSION_DEPTH; ++i) {
                delta[i + 1] = -account.proxied_vsf_votes[i];
            }
            _db.adjust_proxied_witness_votes(account, delta);

            if (o.proxy.size()) {
                const auto &new_proxy = _db.get_account(o.proxy);
                flat_set<account_id_type> proxy_chain({account.id, new_proxy.id
                });
                proxy_chain.reserve(STEEMIT_MAX_PROXY_RECURSION_DEPTH + 1);

                /// check for proxy loops and fail to update the proxy if it would create a loop
                auto cprox = &new_proxy;
                while (cprox->proxy.size() != 0) {
                    const auto next_proxy = _db.get_account(cprox->proxy);
                    FC_ASSERT(proxy_chain.insert(next_proxy.id).second, "This proxy would create a proxy loop.");
                    cprox = &next_proxy;
                    FC_ASSERT(proxy_chain.size() <=
                              STEEMIT_MAX_PROXY_RECURSION_DEPTH, "Proxy chain is too long.");
                }

                /// clear all individual vote records
                _db.clear_witness_votes(account);

                _db.modify(account, [&](account_object &a) {
                    a.proxy = o.proxy;
                });

                /// add all new votes
                for (int i = 0; i <= STEEMIT_MAX_PROXY_RECURSION_DEPTH; ++i) {
                    delta[i] = -delta[i];
                }
                _db.adjust_proxied_witness_votes(account, delta);
            } else { /// we are clearing the proxy which means we simply update the account
                _db.modify(account, [&](account_object &a) {
                    a.proxy = o.proxy;
                });
            }
        }


        void account_witness_vote_evaluator::do_apply(const account_witness_vote_operation &o) {
            database &_db = db();
            const auto &voter = _db.get_account(o.account);
            FC_ASSERT(voter.proxy.size() ==
                      0, "A proxy is currently set, please clear the proxy before voting for a witness.");

            const auto &witness = _db.get_witness(o.witness);

            const auto &by_account_witness_idx = _db.get_index<witness_vote_index>().indices().get<by_account_witness>();
            auto itr = by_account_witness_idx.find(boost::make_tuple(voter.id, witness.id));

            if (itr == by_account_witness_idx.end()) {
                FC_ASSERT(o.approve, "Vote doesn't exist, user must indicate a desire to approve witness.");

                FC_ASSERT(voter.witnesses_voted_for <
                          STEEMIT_MAX_ACCOUNT_WITNESS_VOTES, "Account has voted for too many witnesses."); // TODO: Remove after hardfork 2

                _db.create<witness_vote_object>([&](witness_vote_object &v) {
                    v.witness = witness.id;
                    v.account = voter.id;
                });

                _db.adjust_witness_vote(witness, voter.witness_vote_weight());

                _db.modify(voter, [&](account_object &a) {
                    a.witnesses_voted_for++;
                });

            } else {
                FC_ASSERT(!o.approve, "Vote currently exists, user must indicate a desire to reject witness.");

                _db.adjust_witness_vote(witness, -voter.witness_vote_weight());
                _db.modify(voter, [&](account_object &a) {
                    a.witnesses_voted_for--;
                });
                _db.remove(*itr);
            }
        }

        void vote_evaluator::do_apply(const vote_operation &o) {
            try {
                database &_db = db();

                const auto &comment = _db.get_comment(o.author, o.permlink);
                const auto &voter = _db.get_account(o.voter);

                if (_db.calculate_discussion_payout_time(comment) ==
                    fc::time_point_sec::maximum()) {
                    if(!_db.clear_votes()) {
                        const auto& comment_vote_idx = _db.get_index< comment_vote_index >().indices().get< by_comment_voter >();
                        auto itr = comment_vote_idx.find( std::make_tuple( comment.id, voter.id ) );

                        if( itr == comment_vote_idx.end() )
                            _db.create< comment_vote_object >( [&]( comment_vote_object& cvo ) {
                                cvo.voter = voter.id;
                                cvo.comment = comment.id;
                                cvo.vote_percent = o.weight;
                                cvo.last_update = _db.head_block_time();
                            });
                        else
                            _db.modify( *itr, [&]( comment_vote_object& cvo ) {
                                cvo.vote_percent = o.weight;
                                cvo.last_update = _db.head_block_time();
                        });
                    }
                    return;
                }

                const auto &comment_vote_idx = _db.get_index<comment_vote_index>().indices().get<by_comment_voter>();
                auto itr = comment_vote_idx.find(std::make_tuple(comment.id, voter.id));

                int64_t elapsed_seconds = (_db.head_block_time() -
                                           voter.last_vote_time).to_seconds();

                FC_ASSERT(elapsed_seconds >=
                          STEEMIT_MIN_VOTE_INTERVAL_SEC, "Can only vote once every 1 second.");

                int64_t regenerated_power =
                        (STEEMIT_100_PERCENT * elapsed_seconds) /
                        STEEMIT_VOTE_REGENERATION_SECONDS;
                int64_t current_power = std::min(int64_t(voter.voting_power +
                                                         regenerated_power), int64_t(STEEMIT_100_PERCENT));
                FC_ASSERT(current_power >
                          0, "Account currently does not have voting power.");

                int64_t abs_weight = abs(o.weight);
                int64_t used_power =
                        (current_power * abs_weight) / STEEMIT_100_PERCENT;

                const dynamic_global_property_object &dgpo = _db.get_dynamic_global_properties();

                // used_power = (current_power * abs_weight / STEEMIT_100_PERCENT) * (reserve / max_vote_denom)
                // The second multiplication is rounded up as of HF 259
                int64_t max_vote_denom = dgpo.vote_regeneration_per_day *
                                         STEEMIT_VOTE_REGENERATION_SECONDS /
                                         (60 * 60 * 24);
                FC_ASSERT(max_vote_denom > 0);


                used_power = (used_power + max_vote_denom - 1) / max_vote_denom;
                FC_ASSERT(used_power <=
                          current_power, "Account does not have enough power to vote.");

                int64_t abs_rshares = (
                    (uint128_t(voter.effective_vesting_shares().amount.value) * used_power) /
                    (STEEMIT_100_PERCENT)).to_uint64();

                FC_ASSERT(abs_rshares > 1000000 || o.weight ==
                                                    0, "Voting weight is too small, please accumulate more Shares");

                // Lazily delete vote
                if (itr != comment_vote_idx.end() && itr->num_changes == -1) {
                    if (_db.is_producing())
                        FC_ASSERT(false, "Cannot vote again on a comment after payout.");

                    _db.remove(*itr);
                    itr = comment_vote_idx.end();
                }

                if (itr == comment_vote_idx.end()) {
                    FC_ASSERT(o.weight != 0, "Vote weight cannot be 0.");
                    /// this is the rshares voting for or against the post
                    int64_t rshares = o.weight < 0 ? -abs_rshares : abs_rshares;

                    if (rshares > 0) {
                        FC_ASSERT(_db.head_block_time() <
                                  _db.calculate_discussion_payout_time(comment) - STEEMIT_UPVOTE_LOCKOUT,
                                  "Cannot increase reward of post within the last minute before payout.");
                    }

                    //used_power /= (50*7); /// a 100% vote means use .28% of voting power which should force users to spread their votes around over 50+ posts day for a week
                    //if( used_power == 0 ) used_power = 1;

                    _db.modify(voter, [&](account_object &a) {
                        a.voting_power = current_power - used_power;
                        a.last_vote_time = _db.head_block_time();
                        a.vote_count++;
                    });

                    /// if the current net_rshares is less than 0, the post is getting 0 rewards so it is not factored into total rshares^2
                    fc::uint128_t old_rshares = std::max(comment.net_rshares.value, int64_t(0));

                    FC_ASSERT(abs_rshares > 0, "Cannot vote with 0 rshares.");

                    auto old_vote_rshares = comment.vote_rshares;

                    _db.modify(comment, [&](comment_object &c) {
                        c.net_rshares += rshares;
                        c.abs_rshares += abs_rshares;
                        if (rshares > 0) {
                            c.vote_rshares += rshares;
                        }
                        if (rshares > 0) {
                            c.net_votes++;
                        } else {
                            c.net_votes--;
                        }
                    });

                    fc::uint128_t new_rshares = std::max(comment.net_rshares.value, int64_t(0));

                    uint64_t max_vote_weight = 0;

                   /** this verifies uniqueness of voter
                    *
                    *  cv.weight / c.total_vote_weight ==> % of rshares increase that is accounted for by the vote
                    *
                    *  W(R) = B * R / ( R + 2S )
                    *  W(R) is bounded above by B. B is fixed at 2^64 - 1, so all weights fit in a 64 bit integer.
                    *
                    *  The equation for an individual vote is:
                    *    W(R_N) - W(R_N-1), which is the delta increase of proportional weight
                    *
                    *  c.total_vote_weight =
                    *    W(R_1) - W(R_0) +
                    *    W(R_2) - W(R_1) + ...
                    *    W(R_N) - W(R_N-1) = W(R_N) - W(R_0)
                    *
                    *  Since W(R_0) = 0, c.total_vote_weight is also bounded above by B and will always fit in a 64 bit integer.
                    *
                    **/

                    _db.create<comment_vote_object>([&](comment_vote_object &cv) {
                        cv.voter = voter.id;
                        cv.comment = comment.id;
                        cv.rshares = rshares;
                        cv.vote_percent = o.weight;
                        cv.last_update = _db.head_block_time();

                        if (rshares > 0 && (comment.last_payout == fc::time_point_sec())) {
                            uint64_t old_weight = (
                                    (std::numeric_limits<uint64_t>::max() *
                                     fc::uint128_t(old_vote_rshares.value)) /
                                    (1 + old_vote_rshares.value)).to_uint64();
                            uint64_t new_weight = (
                                    (std::numeric_limits<uint64_t>::max() *
                                     fc::uint128_t(comment.vote_rshares.value)) /
                                    (1 + comment.vote_rshares.value)).to_uint64();
                            cv.weight = new_weight - old_weight;
                            max_vote_weight = cv.weight;
                        } else {
                            cv.weight = 0;
                        }
                    });

                    if (max_vote_weight) // Optimization
                    {
                        _db.modify(comment, [&](comment_object &c) {
                            c.total_vote_weight += max_vote_weight;
                        });
                    }

                    _db.adjust_rshares2(comment, old_rshares, new_rshares);
                } else {
                    FC_ASSERT(itr->num_changes <
                              STEEMIT_MAX_VOTE_CHANGES, "Voter has used the maximum number of vote changes on this comment.");

                    FC_ASSERT(itr->vote_percent !=
                              o.weight, "You have already voted in a similar way.");

                    /// this is the rshares voting for or against the post
                    int64_t rshares = o.weight < 0 ? -abs_rshares : abs_rshares;

                    if (itr->rshares < rshares) {
                        FC_ASSERT(_db.head_block_time() <
                                  _db.calculate_discussion_payout_time(comment) - STEEMIT_UPVOTE_LOCKOUT,
                                  "Cannot increase reward of post within the last minute before payout.");
                    }

                    _db.modify(voter, [&](account_object &a) {
                        a.voting_power = current_power - used_power;
                        a.last_vote_time = _db.head_block_time();
                    });

                    /// if the current net_rshares is less than 0, the post is getting 0 rewards so it is not factored into total rshares^2
                    fc::uint128_t old_rshares = std::max(comment.net_rshares.value, int64_t(0));

                    _db.modify(comment, [&](comment_object &c) {
                        c.net_rshares -= itr->rshares;
                        c.net_rshares += rshares;
                        c.abs_rshares += abs_rshares;

                        /// TODO: figure out how to handle remove a vote (rshares == 0 )
                        if (rshares > 0 && itr->rshares < 0) {
                            c.net_votes += 2;
                        } else if (rshares > 0 && itr->rshares == 0) {
                            c.net_votes += 1;
                        } else if (rshares == 0 && itr->rshares < 0) {
                            c.net_votes += 1;
                        } else if (rshares == 0 && itr->rshares > 0) {
                            c.net_votes -= 1;
                        } else if (rshares < 0 && itr->rshares == 0) {
                            c.net_votes -= 1;
                        } else if (rshares < 0 && itr->rshares > 0) {
                            c.net_votes -= 2;
                        }
                    });

                    fc::uint128_t new_rshares = std::max(comment.net_rshares.value, int64_t(0));

                    _db.modify(comment, [&](comment_object &c) {
                        c.total_vote_weight -= itr->weight;
                    });

                    _db.modify(*itr, [&](comment_vote_object &cv) {
                        cv.rshares = rshares;
                        cv.vote_percent = o.weight;
                        cv.last_update = _db.head_block_time();
                        cv.weight = 0;
                        cv.num_changes += 1;
                    });

                    _db.adjust_rshares2(comment, old_rshares, new_rshares);
                }

            } FC_CAPTURE_AND_RETHROW((o))
        }

        void custom_json_evaluator::do_apply(const custom_json_operation &o) {
            database &d = db();
            std::shared_ptr<custom_operation_interpreter> eval = d.get_custom_json_evaluator(o.id);
            if (!eval) {
                return;
            }

            try {
                eval->apply(o);
            }
            catch (const fc::exception &e) {
                if (d.is_producing()) {
                    throw e;
                }
            }
            catch (...) {
                elog("Unexpected exception applying custom json evaluator.");
            }
        }

        void request_account_recovery_evaluator::do_apply(const request_account_recovery_operation &o) {
            database &_db = db();
            const auto &account_to_recover = _db.get_account(o.account_to_recover);

            if (account_to_recover.recovery_account.length())   // Make sure recovery matches expected recovery account
                FC_ASSERT(account_to_recover.recovery_account ==
                          o.recovery_account, "Cannot recover an account that does not have you as there recovery partner.");
            else                                                  // Empty string recovery account defaults to top witness
                FC_ASSERT(
                        _db.get_index<witness_index>().indices().get<by_vote_name>().begin()->owner ==
                        o.recovery_account, "Top witness must recover an account with no recovery partner.");

            const auto &recovery_request_idx = _db.get_index<account_recovery_request_index>().indices().get<by_account>();
            auto request = recovery_request_idx.find(o.account_to_recover);

            if (request == recovery_request_idx.end()) // New Request
            {
                FC_ASSERT(!o.new_owner_authority.is_impossible(), "Cannot recover using an impossible authority.");
                FC_ASSERT(o.new_owner_authority.weight_threshold, "Cannot recover using an open authority.");

                // Check accounts in the new authority exist
                for (auto &a : o.new_owner_authority.account_auths) {
                    _db.get_account(a.first);
                }

                _db.create<account_recovery_request_object>([&](account_recovery_request_object &req) {
                    req.account_to_recover = o.account_to_recover;
                    req.new_owner_authority = o.new_owner_authority;
                    req.expires = _db.head_block_time() +
                                  STEEMIT_ACCOUNT_RECOVERY_REQUEST_EXPIRATION_PERIOD;
                });
            } else if (o.new_owner_authority.weight_threshold ==
                       0) // Cancel Request if authority is open
            {
                _db.remove(*request);
            } else // Change Request
            {
                FC_ASSERT(!o.new_owner_authority.is_impossible(), "Cannot recover using an impossible authority.");

                // Check accounts in the new authority exist
                for (auto &a : o.new_owner_authority.account_auths) {
                    _db.get_account(a.first);
                }

                _db.modify(*request, [&](account_recovery_request_object &req) {
                    req.new_owner_authority = o.new_owner_authority;
                    req.expires = _db.head_block_time() +
                                  STEEMIT_ACCOUNT_RECOVERY_REQUEST_EXPIRATION_PERIOD;
                });
            }
        }

        void recover_account_evaluator::do_apply(const recover_account_operation &o) {
            database &_db = db();
            const auto &account = _db.get_account(o.account_to_recover);

            FC_ASSERT(
                    _db.head_block_time() - account.last_account_recovery >
                    STEEMIT_OWNER_UPDATE_LIMIT, "Owner authority can only be updated once an hour.");

            const auto &recovery_request_idx = _db.get_index<account_recovery_request_index>().indices().get<by_account>();
            auto request = recovery_request_idx.find(o.account_to_recover);

            FC_ASSERT(request !=
                      recovery_request_idx.end(), "There are no active recovery requests for this account.");
            FC_ASSERT(request->new_owner_authority ==
                      o.new_owner_authority, "New owner authority does not match recovery request.");

            const auto &recent_auth_idx = _db.get_index<owner_authority_history_index>().indices().get<by_account>();
            auto hist = recent_auth_idx.lower_bound(o.account_to_recover);
            bool found = false;

            while (hist != recent_auth_idx.end() &&
                   hist->account == o.account_to_recover && !found) {
                found = hist->previous_owner_authority ==
                        o.recent_owner_authority;
                if (found) {
                    break;
                }
                ++hist;
            }

            FC_ASSERT(found, "Recent authority not found in authority history.");

            _db.remove(*request); // Remove first, update_owner_authority may invalidate iterator
            _db.update_owner_authority(account, o.new_owner_authority);
            _db.modify(account, [&](account_object &a) {
                a.last_account_recovery = _db.head_block_time();
            });
        }

        void change_recovery_account_evaluator::do_apply(const change_recovery_account_operation &o) {
            database &_db = db();
            _db.get_account(o.new_recovery_account); // Simply validate account exists
            const auto &account_to_recover = _db.get_account(o.account_to_recover);

            const auto &change_recovery_idx = _db.get_index<change_recovery_account_request_index>().indices().get<by_account>();
            auto request = change_recovery_idx.find(o.account_to_recover);

            if (request == change_recovery_idx.end()) // New request
            {
                _db.create<change_recovery_account_request_object>([&](change_recovery_account_request_object &req) {
                    req.account_to_recover = o.account_to_recover;
                    req.recovery_account = o.new_recovery_account;
                    req.effective_on = _db.head_block_time() +
                                       STEEMIT_OWNER_AUTH_RECOVERY_PERIOD;
                });
            } else if (account_to_recover.recovery_account !=
                       o.new_recovery_account) // Change existing request
            {
                _db.modify(*request, [&](change_recovery_account_request_object &req) {
                    req.recovery_account = o.new_recovery_account;
                    req.effective_on = _db.head_block_time() +
                                       STEEMIT_OWNER_AUTH_RECOVERY_PERIOD;
                });
            } else // Request exists and changing back to current recovery account
            {
                _db.remove(*request);
            }
        }

        void delegate_vesting_shares_evaluator::do_apply(const delegate_vesting_shares_operation& op) {
            const auto& delegator = _db.get_account(op.delegator);
            const auto& delegatee = _db.get_account(op.delegatee);
            auto delegation = _db.find<vesting_delegation_object, by_delegation>(std::make_tuple(op.delegator, op.delegatee));

            const auto& median_props = _db.get_witness_schedule_object().median_props;
            const auto v_share_price = _db.get_dynamic_global_properties().get_vesting_share_price();
            auto min_delegation = median_props.min_delegation * v_share_price;

            auto now = _db.head_block_time();
            auto delta = delegation ?
                op.vesting_shares - delegation->vesting_shares :
                op.vesting_shares;
            auto increasing = delta.amount > 0;

            if (increasing) {
                auto delegated = delegator.delegated_vesting_shares;
                FC_ASSERT(delegator.available_vesting_shares(true) >= delta,
                    "Account does not have enough vesting shares to delegate.",
                    ("available", delegator.available_vesting_shares(true))
                    ("delta", delta)("vesting_shares", delegator.vesting_shares)("delegated", delegated)
                    ("to_withdraw", delegator.to_withdraw)("withdrawn", delegator.withdrawn));
                auto elapsed_seconds = (now - delegator.last_vote_time).to_seconds();
                auto regenerated_power = (STEEMIT_100_PERCENT * elapsed_seconds) / STEEMIT_VOTE_REGENERATION_SECONDS;
                auto current_power = std::min<int64_t>(delegator.voting_power + regenerated_power, STEEMIT_100_PERCENT);
                auto max_allowed = delegator.vesting_shares * current_power / STEEMIT_100_PERCENT;
                FC_ASSERT(delegated + delta <= max_allowed,
                    "Account allowed to delegate a maximum of ${v} with current voting power = ${p}",
                    ("v",max_allowed)("p",current_power)("delegated",delegated)("delta",delta));

                if (!delegation) {
                    FC_ASSERT(op.vesting_shares >= min_delegation,
                        "Account must delegate a minimum of ${v}", ("v",min_delegation)("vesting_shares",op.vesting_shares));
                    _db.create<vesting_delegation_object>([&](vesting_delegation_object& o) {
                        o.delegator = op.delegator;
                        o.delegatee = op.delegatee;
                        o.vesting_shares = op.vesting_shares;
                        o.min_delegation_time = now;
                    });
                }
                _db.modify(delegator, [&](account_object& a) {
                    a.delegated_vesting_shares += delta;
                });
            } else {
                FC_ASSERT(op.vesting_shares.amount == 0 || op.vesting_shares >= min_delegation,
                    "Delegation must be removed or leave minimum delegation amount of ${v}",
                    ("v",min_delegation)("vesting_shares",op.vesting_shares));
                _db.create<vesting_delegation_expiration_object>([&](vesting_delegation_expiration_object& o) {
                    o.delegator = op.delegator;
                    o.vesting_shares = -delta;
                    o.expiration = std::max(now + STEEMIT_CASHOUT_WINDOW_SECONDS, delegation->min_delegation_time);
                });
            }

            _db.modify(delegatee, [&](account_object& a) {
                a.received_vesting_shares += delta;
            });
            if (delegation) {
                if (op.vesting_shares.amount > 0) {
                    _db.modify(*delegation, [&](vesting_delegation_object& o) {
                        o.vesting_shares = op.vesting_shares;
                    });
                } else {
                    _db.remove(*delegation);
                }
            }
        }

} } // golos::chain
