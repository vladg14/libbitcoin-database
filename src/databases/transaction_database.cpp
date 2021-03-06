/**
 * Copyright (c) 2011-2017 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/database/databases/transaction_database.hpp>

#include <cstddef>
#include <cstdint>
#include <boost/filesystem.hpp>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/database/define.hpp>
#include <bitcoin/database/memory/memory.hpp>
#include <bitcoin/database/result/transaction_result.hpp>
#include <bitcoin/database/state/transaction_state.hpp>

namespace libbitcoin {
namespace database {

using namespace bc::chain;
using namespace bc::machine;

// Record format (v4):
// ----------------------------------------------------------------------------
// [ height/forks/code:4 - atomic1 ] (code if invalid)
// [ position:2          - atomic1 ] (unconfirmed sentinel, could store state)
// [ state:1             - atomic1 ] (invalid, stored, pooled, indexed, confirmed)
// [ median_time_past:4  - atomic1 ] (zero if unconfirmed)
// [ output_count:varint - const   ] (tx starts here)
// [
//   [ index_spend:1    - atomic2 ]
//   [ spender_height:4 - atomic2 ] (could store index_spend in high bit)
//   [ value:8          - const   ]
//   [ script:varint    - const   ]
// ]...
// [ input_count:varint   - const   ]
// [
//   [ hash:32           - const  ]
//   [ index:2           - const  ]
//   [ script:varint     - const  ]
//   [ sequence:4        - const  ]
// ]...
// [ locktime:varint      - const   ]
// [ version:varint       - const   ]

// Record format (v3.3):
// ----------------------------------------------------------------------------
// [ height/forks:4         - atomic1 ]
// [ position/unconfirmed:2 - atomic1 ]
// [ median_time_past:4     - atomic1 ]
// [ output_count:varint    - const   ]
// [ [ spender_height:4 - atomic2 ][ value:8 ][ script:varint ] ]...
// [ input_count:varint     - const   ]
// [ [ hash:32 ][ index:2 ][ script:varint ][ sequence:4 ] ]...
// [ locktime:varint        - const   ]
// [ version:varint         - const   ]

static constexpr auto height_size = sizeof(uint32_t);
static constexpr auto position_size = sizeof(uint16_t);
static constexpr auto state_size = sizeof(uint8_t);
static constexpr auto median_time_past_size = sizeof(uint32_t);

static constexpr auto index_spend_size = sizeof(uint8_t);
////static constexpr auto height_size = sizeof(uint32_t);
static constexpr auto value_size = sizeof(uint64_t);

static constexpr auto spend_size = index_spend_size + height_size + value_size;
static constexpr auto metadata_size = height_size + position_size +
    state_size + median_time_past_size;

static constexpr auto no_time = 0u;

// Transactions uses a hash table index, O(1).
transaction_database::transaction_database(const path& map_filename,
    size_t buckets, size_t expansion, size_t cache_capacity)
  : hash_table_file_(map_filename, expansion),
    hash_table_(hash_table_file_, buckets),
    cache_(cache_capacity)
{
}

transaction_database::~transaction_database()
{
    close();
}

// Startup and shutdown.
// ----------------------------------------------------------------------------

bool transaction_database::create()
{
    if (!hash_table_file_.open())
        return false;

    // No need to call open after create.
    return
        hash_table_.create();
}

bool transaction_database::open()
{
    return
        hash_table_file_.open() &&
        hash_table_.start();
}

void transaction_database::commit()
{
    hash_table_.commit();
}

bool transaction_database::flush() const
{
    return hash_table_file_.flush();
}

bool transaction_database::close()
{
    return hash_table_file_.close();
}

// Queries.
// ----------------------------------------------------------------------------

transaction_result transaction_database::get(file_offset offset) const
{
    // This is not guarded for an invalid offset.
    return { hash_table_.find(offset), metadata_mutex_ };
}

transaction_result transaction_database::get(const hash_digest& hash) const
{
    return { hash_table_.find(hash), metadata_mutex_ };
}

// Metadata should be defaulted by caller.
// Set fork_height to max_size_t for tx pool metadata.
bool transaction_database::get_output(const output_point& point,
    size_t fork_height) const
{
    auto& prevout = point.metadata;
    prevout.height = 0;
    prevout.median_time_past = 0;
    prevout.spent = false;

    // If the input is a coinbase there is no prevout to populate.
    if (point.is_null())
        return false;

    // Cache does not contain spent outputs or indexed confirmation states.
    if (cache_.populate(point, fork_height))
        return true;

    // Find the tx entry.
    const auto result = get(point.hash());

    if (!result)
        return false;

    //*************************************************************************
    // CONSENSUS: The genesis block coinbase output may not be spent. This is
    // the consequence of satoshi not including it in the utxo set for block
    // database initialization. Only he knows why, probably an oversight.
    //*************************************************************************
    const auto height = result.height();
    if (height == 0)
        return false;

    const auto state = result.state();
    const auto relevant = height <= fork_height;
    const auto for_pool = fork_height == max_size_t;

    const auto confirmed = 
        (state == transaction_state::indexed && !for_pool) ||
        (state == transaction_state::confirmed && relevant);

    // Guarantee confirmation state.
    if (!for_pool && !confirmed)
        return false;

    // Find the output at the specified index for the found tx.
    prevout.cache = result.output(point.index());
    if (!prevout.cache.is_valid())
        return false;

    // Populate the output metadata.
    prevout.confirmed = confirmed;
    prevout.coinbase = result.position() == 0;

    prevout.height = height;
    prevout.median_time_past = result.median_time_past();
    prevout.spent = prevout.confirmed && prevout.cache.metadata.spent(
        fork_height);

    // Return is redundant with cache validity.
    return true;
}

// Store.
// ----------------------------------------------------------------------------

bool transaction_database::store(const chain::transaction& tx, uint32_t height,
    uint32_t median_time_past, size_t position, transaction_state state)
{
    BITCOIN_ASSERT(height <= max_uint32);
    BITCOIN_ASSERT(position <= max_uint16);

    const auto writer = [&](byte_serializer& serial)
    {
        serial.write_4_bytes_little_endian(static_cast<uint32_t>(height));
        serial.write_2_bytes_little_endian(static_cast<uint16_t>(position));
        serial.write_byte(static_cast<uint8_t>(state));
        serial.write_4_bytes_little_endian(median_time_past);
        tx.to_data(serial, false, true);
    };

    // Transactions are variable-sized.
    const auto size = metadata_size + tx.serialized_size(false, true);

    // Write the new transaction.
    auto next = hash_table_.allocator();
    tx.metadata.link = next.create(tx.hash(), writer, size);
    hash_table_.link(next);
    return true;
}

bool transaction_database::pool(const chain::transaction& tx, uint32_t forks)
{
    return store(tx, forks, no_time, transaction_result::unconfirmed,
        transaction_state::pooled);
}

// Update.
// ----------------------------------------------------------------------------

// private
bool transaction_database::unspend(const output_point& point)
{
    return spend(point, output::validation::not_spent);
}

// private
bool transaction_database::spend(const output_point& point,
    size_t spender_height)
{
    // This just simplifies calling by allowing coinbase to be included.
    if (point.is_null())
        return true;

    // If unspending we could restore the spend to the cache, but not worth it.
    if (spender_height != output::validation::not_spent)
        cache_.remove(point);

    auto element = hash_table_.find(point.hash());

    if (!element)
        return false;

    uint32_t height;
    transaction_state state;
    size_t outputs;
    const auto reader = [&](byte_deserializer& deserial)
    {
        // Critical Section
        ///////////////////////////////////////////////////////////////////////
        shared_lock lock(metadata_mutex_);
        height = deserial.read_4_bytes_little_endian();
        deserial.skip(position_size);
        state = static_cast<transaction_state>(deserial.read_byte());
        deserial.skip(median_time_past_size);
        outputs = deserial.read_size_little_endian();
        ///////////////////////////////////////////////////////////////////////
    };

    element.read(reader);

    // Limit to confirmed transactions at or below the spender height.
    if (state != transaction_state::confirmed || height > spender_height)
        return false;

    // The index is not in the transaction.
    if (point.index() >= outputs)
        return false;

    const auto writer = [&](byte_serializer& serial)
    {
        serial.skip(metadata_size);
        serial.read_size_little_endian();

        // Skip outputs until the target output.
        for (uint32_t output = 0; output < point.index(); ++output)
        {
            serial.skip(spend_size);
            serial.skip(serial.read_size_little_endian());
        }

        serial.skip(index_spend_size);

        // Critical Section
        ///////////////////////////////////////////////////////////////////////
        unique_lock lock(metadata_mutex_);
        serial.write_4_bytes_little_endian(spender_height);
        ///////////////////////////////////////////////////////////////////////
    };

    element.write(writer);
    return true;
}

bool transaction_database::unconfirm(file_offset link)
{
    const auto result = get(link);

    if (!result)
        return false;

    for (const auto inpoint: result)
        if (!unspend(inpoint))
            return false;

    // The tx was verified under a now unknown chain state, so set unverified.
    return update(link, rule_fork::unverified, no_time,
        transaction_result::unconfirmed, transaction_state::pooled);
}

bool transaction_database::confirm(file_offset link, size_t height,
    uint32_t median_time_past, size_t position)
{
    BITCOIN_ASSERT(height <= max_uint32);
    BITCOIN_ASSERT(position <= max_uint16);
    BITCOIN_ASSERT(position != transaction_result::unconfirmed);

    const auto result = get(link);

    if (!result)
        return false;

    // Spend the tx's previous outputs.
    for (const auto inpoint: result)
        if (!spend(inpoint, height))
            return false;

    // TODO: consider eliminating the output cache.
    // TODO: It may be more costly to populate it than the benefit, because txs
    // will have to be read from disk and loaded into the cache!
    ////// Promote the tx that already exists.
    ////if (link != slab_map::not_found)
    ////{
    ////    cache_.add(tx, height, median_time_past, true);
    ////    return confirm(tx.metadata.link, height, median_time_past,
    ////        position, transaction_state::confirmed);
    ////}

    return update(link, height, median_time_past, position,
        transaction_state::confirmed);
}

// private
bool transaction_database::update(link_type link, size_t height,
    uint32_t median_time_past, size_t position, transaction_state state)
{
    BITCOIN_ASSERT(height <= max_uint32);
    BITCOIN_ASSERT(position <= max_uint16);
    auto element = hash_table_.find(link);

    if (!element)
        return false;

    const auto writer = [&](byte_serializer& serial)
    {
        // Critical Section
        ///////////////////////////////////////////////////////////////////////
        unique_lock lock(metadata_mutex_);
        serial.write_4_bytes_little_endian(static_cast<uint32_t>(height));
        serial.write_2_bytes_little_endian(static_cast<uint16_t>(position));
        serial.write_byte(static_cast<uint8_t>(state));
        serial.write_4_bytes_little_endian(median_time_past);
        ///////////////////////////////////////////////////////////////////////
    };

    element.write(writer);
    return true;
}

} // namespace database
} // namespace libbitcoin
