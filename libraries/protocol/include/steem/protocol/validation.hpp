
#pragma once

#include <steem/protocol/base.hpp>
#include <steem/protocol/block_header.hpp>
#include <steem/protocol/asset.hpp>

#include <fc/utf8.hpp>

namespace steem { namespace protocol {

inline bool is_asset_type( asset asset, asset_symbol_type symbol )
{
   return asset.symbol == symbol;
}

inline void validate_account_name( const string& name )
{
   FC_ASSERT( is_valid_account_name( name ), "Account name ${n} is invalid", ("n", name) );
}

inline void validate_create_account_name( const string& name )
{
   FC_ASSERT( is_valid_create_account_name( name ), "Account name ${n} is invalid", ("n", name) );
}

inline void validate_domain_name( const string& name, const string& creator )
{
   FC_ASSERT( is_valid_domain_name( name, creator ), "Domain name ${n} is invalid, creator name ${c}", ("n", name)("c", creator) );
}

inline void validate_permlink( const string& permlink )
{
   FC_ASSERT( permlink.size() < STEEM_MAX_PERMLINK_LENGTH, "permlink is too long" );
   FC_ASSERT( fc::is_utf8( permlink ), "permlink not formatted in UTF8" );
}

} }
