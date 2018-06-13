// Copyright (c) 2018 The Merit Foundation developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pog2/anv.h"
#include "addressindex.h"
#include "validation.h"

#include <stack>
#include <deque>
#include <numeric>

namespace pog2
{
    /**
     * This version simply pulls the ANV from the DB. ReferralsViewDB::UpdateANV
     * incrementally updates an ANV for an address and all parents.
     */
    referral::MaybeAddressANV ComputeANV(
            const referral::Address& address_id,
            const referral::ReferralsViewDB& db)
    {
        return db.GetANV(address_id);
    }

    referral::AddressANVs GetAllANVs(const referral::ReferralsViewDB& db)
    {
        return db.GetAllANVs();
    }

    void GetAllRewardableANVs(
            const referral::ReferralsViewDB& db,
            const Consensus::Params& params,
            int height,
            referral::AddressANVs& entrants)
    {
        db.GetAllRewardableANVs(params, height, entrants);
    }

    referral::AddressANVs GetANVs(
            const referral::Addresses& addresses,
            const referral::ReferralsViewDB& db)
    {
        referral::AddressANVs r;
        r.reserve(addresses.size());

        for(const auto& a : addresses) {
            if(auto maybe_anv = ComputeANV(a, db)) {
                r.push_back(*maybe_anv);
            }
        }

        assert(r.size() <= addresses.size());
        return r;
    }

    struct Coin
    {
        int height;
        CAmount amount;
    };

    using Coins = std::vector<Coin>;
    using AddressIndexPair = std::pair<CAddressIndexKey, CAmount>;

    Coins GetCoins(char address_type, const referral::Address& address) {
        std::vector<AddressIndexPair> address_index;
        if (!GetAddressIndex(address, address_type, false, address_index)) {
            return {};
        }

        Coins cs(address_index.size());
        std::transform(address_index.begin(), address_index.end(), cs.begin(),
                [](const AddressIndexPair& p) {
                    return Coin {
                        p.first.blockHeight,
                        p.second};
                });
        return cs;
    }

    double Age(int height, const Coin& c) {
        assert(height >= 0);
        assert(c.height <= height);

        return height - c.height;
    }

    const double ONE_DAY = 24*60;
    double AgeScale(int height, const Coin& c) {
        assert(height >= 0);
        assert(c.height <= height);

        double age = Age(height, c) / ONE_DAY;
        assert(age >= 0);

        double scale =  1.0 - (1.0 / std::pow(age, 2) + 1);

        assert(scale >= 0);
        assert(scale <= 1);
        return scale;
    }

    CAmount AgedBalance(int height, const Coin& c) {
        assert(height >= 0);
        assert(c.height <= height);

        double age_scale = AgeScale(height, c);
        CAmount amount = std::floor(age_scale * c.amount);

        assert(amount >= 0);
        assert(amount <= c.amount);
        return amount;
    }

    CAmount AgedBalance(int height, const Coins& cs) {
        assert(height >= 0);

        return std::accumulate(cs.begin(), cs.end(), CAmount{0}, 
                [height](int amount, const Coin& c) {
                    return amount + AgedBalance(height, c);
                });
    }

    struct TreeNode
    {
        char address_type;
        referral::Address address;
        int level;
    };

    using TreeNodeQueue = std::deque<TreeNode>; 

    void PushChildren(
            const referral::ReferralsViewDB& db,
            const TreeNode& n,
            TreeNodeQueue& q) 
    {
        auto children = db.GetChildren(n.address);
        for(const auto& address : children) {
            auto maybe_ref = db.GetReferral(address);
            if(!maybe_ref) { 
                continue;
            }

            q.push_back({
                    maybe_ref->addressType,
                    maybe_ref->GetAddress(),
                    n.level + 1});
        }
    }

    referral::MaybeAddressANV ComputeCGS(
            int height,
            char address_type,
            const referral::Address& address,
            const referral::ReferralsViewDB& db)
    {
        auto coins = GetCoins(address_type, address);
        auto balance = AgedBalance(height, coins);

        CAmount gcs = std::floor(balance * 0.75);

        TreeNodeQueue q;

        TreeNode root{address_type, address, 1};
        PushChildren(db, root, q);

        while(!q.empty()) {
            auto n = q.front();
            q.pop_front();
            auto node_coins = GetCoins(n.address_type, n.address);
            auto node_balance = AgedBalance(height, node_coins);
            gcs += node_balance / n.level;

            PushChildren(db, n, q);
        }

        return referral::AddressANV{address_type, address, gcs};
    }

} // namespace pog2
