#include <iterator>

#include <fc/io/datastream.hpp>

#include <graphene/chain/proposal_object.hpp>
#include <graphene/chain/account_object.hpp>
#include <graphene/chain/database.hpp>

#include <graphene/protocol/exceptions.hpp>

namespace graphene { namespace chain {

    namespace {
        template <typename S> static fc::flat_set<typename S::value_type> copy_to_heap(const S& src) {
            return fc::flat_set<typename S::value_type>(src.begin(), src.end());
        }

        template <typename S1, typename S2> static fc::flat_set<typename S1::value_type> copy_to_heap(
            const S1& src, const S2& src_to_add
        ) {
            auto result = copy_to_heap(src);
            result.insert(src_to_add.begin(), src_to_add.end());
            return result;
        }
    }

    std::vector<protocol::operation> proposal_object::operations() const {
        std::vector<protocol::operation> operations;

        fc::datastream<const char *> ds(proposed_operations.data(), proposed_operations.size());
        fc::raw::unpack(ds, operations);

        return operations;
    }

    bool proposal_object::is_authorized_to_execute(const database& db) const {
        try {
            verify_authority(db);
        } catch (const protocol::tx_irrelevant_sig& e) {
            // not critical, because it is the last step of verify
        } catch (const protocol::tx_irrelevant_approval& e) {
            // not critical, because it is the last step of verify
        } catch (const fc::exception& e) {
            return false;
        }
        return true;
    }

    void proposal_object::verify_authority(
        const database& db,
        const fc::flat_set<account_name_type>& active_approvals_to_add,
        const fc::flat_set<account_name_type>& master_approvals_to_add,
        const fc::flat_set<account_name_type>& regular_approvals_to_add
    ) const {
        auto active_approvals = copy_to_heap(available_active_approvals, active_approvals_to_add);
        auto master_approvals = copy_to_heap(available_master_approvals, master_approvals_to_add);
        auto regular_approvals = copy_to_heap(available_regular_approvals, regular_approvals_to_add);
        auto key_approvals = copy_to_heap(available_key_approvals);
        auto ops = operations();

        auto get_active = [&](const account_name_type& name) {
            return authority(db.get<account_authority_object, by_account>(name).active);
        };

        auto get_master = [&](const account_name_type& name) {
            return authority(db.get<account_authority_object, by_account>(name).master);
        };

        auto get_regular = [&](const account_name_type& name) {
            return authority(db.get<account_authority_object, by_account>(name).regular);
        };

        graphene::protocol::verify_authority(
            ops, key_approvals,
            get_active, get_master, get_regular, CHAIN_MAX_SIG_CHECK_DEPTH, false, /* allow committeee */
            active_approvals, master_approvals, regular_approvals);
    }

} } // graphene::chain