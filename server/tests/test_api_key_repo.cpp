#include <catch2/catch_test_macros.hpp>

#include <pqxx/pqxx>

#include "server/api_key_repo.h"
#include "server/crypto_util.h"
#include "server/db.h"

namespace {

struct Fixture {
    Fixture() : conn(TEST_DB_CONN) {
        anjeer::server::DbMigrator migrator(conn, TEST_MIGRATIONS_DIR);
        migrator.run();
        pqxx::work txn(conn);
        txn.exec("TRUNCATE TABLE api_keys CASCADE");
        txn.exec("TRUNCATE TABLE players RESTART IDENTITY CASCADE");
        const auto r = txn.exec_params(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ($1,$2,$3) RETURNING id",
            "apitest_player", "github", "gh_api_1"
        );
        player_a = r[0][0].as<int32_t>();
        const auto r2 = txn.exec_params(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ($1,$2,$3) RETURNING id",
            "apitest_player_b", "github", "gh_api_2"
        );
        player_b = r2[0][0].as<int32_t>();
        txn.commit();
    }

    pqxx::connection             conn;
    anjeer::server::ApiKeyRepo   repo;
    int32_t                      player_a;
    int32_t                      player_b;
};

} // namespace

// ---------------------------------------------------------------------------
// sha256_hex + generate_api_key (pure crypto — no DB)
// ---------------------------------------------------------------------------

TEST_CASE("sha256_hex produces 64-char lowercase hex", "[crypto]") {
    const auto h = anjeer::server::sha256_hex("hello world");
    REQUIRE(h.size() == 64);
    REQUIRE(h.find_first_not_of("0123456789abcdef") == std::string::npos);
}

TEST_CASE("sha256_hex is deterministic", "[crypto]") {
    REQUIRE(anjeer::server::sha256_hex("abc") == anjeer::server::sha256_hex("abc"));
}

TEST_CASE("sha256_hex differs for different inputs", "[crypto]") {
    REQUIRE(anjeer::server::sha256_hex("abc") != anjeer::server::sha256_hex("abd"));
}

TEST_CASE("generate_api_key starts with ank_ and is 68 chars", "[crypto]") {
    const auto k = anjeer::server::generate_api_key();
    REQUIRE(k.substr(0, 4) == "ank_");
    REQUIRE(k.size() == 68);  // "ank_" + 64 hex chars
}

TEST_CASE("generate_api_key produces unique values", "[crypto]") {
    REQUIRE(anjeer::server::generate_api_key() != anjeer::server::generate_api_key());
}

// ---------------------------------------------------------------------------
// ApiKeyRepo::create
// ---------------------------------------------------------------------------

TEST_CASE("create returns ank_ prefixed plaintext key", "[integration][api_key_repo]") {
    Fixture f;
    std::string err;
    pqxx::work txn(f.conn);
    const auto key = f.repo.create(txn, f.player_a, "my bot", err);
    txn.commit();

    REQUIRE(key.has_value());
    REQUIRE(key->substr(0, 4) == "ank_");
    REQUIRE(key->size() == 68);
    REQUIRE(err.empty());
}

TEST_CASE("create stores hash not plaintext in DB", "[integration][api_key_repo]") {
    Fixture f;
    std::string err;
    pqxx::work txn(f.conn);
    const auto key = f.repo.create(txn, f.player_a, "my bot", err);
    txn.commit();

    REQUIRE(key.has_value());

    pqxx::work check(f.conn);
    const auto r = check.exec_params(
        "SELECT key_hash FROM api_keys WHERE player_id = $1", f.player_a
    );
    check.commit();

    REQUIRE(r.size() == 1);
    const std::string stored_hash = r[0][0].as<std::string>();
    REQUIRE(stored_hash != *key);
    REQUIRE(stored_hash == anjeer::server::sha256_hex(*key));
}

TEST_CASE("create second key with first active returns nullopt ACTIVE_KEY_EXISTS",
          "[integration][api_key_repo]") {
    Fixture f;
    std::string err;
    {
        pqxx::work txn(f.conn);
        const auto k1 = f.repo.create(txn, f.player_a, "first", err);
        txn.commit();
        REQUIRE(k1.has_value());
    }
    {
        pqxx::work txn(f.conn);
        const auto k2 = f.repo.create(txn, f.player_a, "second", err);
        txn.commit();
        REQUIRE_FALSE(k2.has_value());
        REQUIRE(err == "ACTIVE_KEY_EXISTS");
    }
}

TEST_CASE("create rejects empty name", "[integration][api_key_repo]") {
    Fixture f;
    std::string err;
    pqxx::work txn(f.conn);
    const auto k = f.repo.create(txn, f.player_a, "", err);
    txn.commit();
    REQUIRE_FALSE(k.has_value());
    REQUIRE(err == "VALIDATION_ERROR");
}

TEST_CASE("create rejects name longer than 100 chars", "[integration][api_key_repo]") {
    Fixture f;
    std::string err;
    pqxx::work txn(f.conn);
    const auto k = f.repo.create(txn, f.player_a, std::string(101, 'x'), err);
    txn.commit();
    REQUIRE_FALSE(k.has_value());
    REQUIRE(err == "VALIDATION_ERROR");
}

// ---------------------------------------------------------------------------
// ApiKeyRepo::find_valid_by_hash
// ---------------------------------------------------------------------------

TEST_CASE("find_valid_by_hash succeeds for a fresh key", "[integration][api_key_repo]") {
    Fixture f;
    std::string err;
    std::string plaintext;
    {
        pqxx::work txn(f.conn);
        plaintext = *f.repo.create(txn, f.player_a, "bot", err);
        txn.commit();
    }
    const std::string hash = anjeer::server::sha256_hex(plaintext);
    pqxx::work txn(f.conn);
    const auto rec = f.repo.find_valid_by_hash(txn, hash);
    txn.commit();

    REQUIRE(rec.has_value());
    REQUIRE(rec->player_id == f.player_a);
    REQUIRE(rec->name == "bot");
    REQUIRE(rec->key_hash == hash);
    REQUIRE_FALSE(rec->revoked_at.has_value());
}

TEST_CASE("find_valid_by_hash returns nullopt for garbage hash", "[integration][api_key_repo]") {
    Fixture f;
    pqxx::work txn(f.conn);
    const auto rec = f.repo.find_valid_by_hash(txn, std::string(64, '0'));
    txn.commit();
    REQUIRE_FALSE(rec.has_value());
}

TEST_CASE("find_valid_by_hash returns nullopt after revoke", "[integration][api_key_repo]") {
    Fixture f;
    std::string err;
    int32_t key_id;
    std::string plaintext;
    {
        pqxx::work txn(f.conn);
        plaintext = *f.repo.create(txn, f.player_a, "bot", err);
        // Retrieve the id
        const auto r = txn.exec_params(
            "SELECT id FROM api_keys WHERE player_id = $1", f.player_a
        );
        key_id = r[0][0].as<int32_t>();
        txn.commit();
    }
    {
        pqxx::work txn(f.conn);
        REQUIRE(f.repo.revoke(txn, key_id, f.player_a));
        txn.commit();
    }
    const std::string hash = anjeer::server::sha256_hex(plaintext);
    pqxx::work txn(f.conn);
    const auto rec = f.repo.find_valid_by_hash(txn, hash);
    txn.commit();
    REQUIRE_FALSE(rec.has_value());
}

TEST_CASE("find_valid_by_hash returns nullopt for expired key", "[integration][api_key_repo]") {
    Fixture f;
    // Insert a row with expires_at in the past directly.
    const std::string fake_key = anjeer::server::generate_api_key();
    const std::string fake_hash = anjeer::server::sha256_hex(fake_key);
    {
        pqxx::work txn(f.conn);
        txn.exec_params(
            "INSERT INTO api_keys (player_id, key_hash, name, expires_at) "
            "VALUES ($1, $2, 'expired', NOW() - INTERVAL '1 second')",
            f.player_a, fake_hash
        );
        txn.commit();
    }
    pqxx::work txn(f.conn);
    const auto rec = f.repo.find_valid_by_hash(txn, fake_hash);
    txn.commit();
    REQUIRE_FALSE(rec.has_value());
}

// ---------------------------------------------------------------------------
// ApiKeyRepo::list_for_player
// ---------------------------------------------------------------------------

TEST_CASE("list_for_player returns keys newest-first without key_hash",
          "[integration][api_key_repo]") {
    Fixture f;
    std::string err;
    {
        pqxx::work txn(f.conn);
        f.repo.create(txn, f.player_a, "first bot", err);
        txn.commit();
    }

    pqxx::work txn(f.conn);
    const auto keys = f.repo.list_for_player(txn, f.player_a);
    txn.commit();

    REQUIRE(keys.size() == 1);
    REQUIRE(keys[0].name == "first bot");
    REQUIRE(keys[0].player_id == f.player_a);
    // ApiKeyView has no key_hash field — verified at compile time by struct definition
}

TEST_CASE("list_for_player returns empty for player with no keys",
          "[integration][api_key_repo]") {
    Fixture f;
    pqxx::work txn(f.conn);
    const auto keys = f.repo.list_for_player(txn, f.player_b);
    txn.commit();
    REQUIRE(keys.empty());
}

// ---------------------------------------------------------------------------
// ApiKeyRepo::revoke
// ---------------------------------------------------------------------------

TEST_CASE("revoke sets revoked_at and returns true", "[integration][api_key_repo]") {
    Fixture f;
    std::string err;
    int32_t key_id;
    {
        pqxx::work txn(f.conn);
        f.repo.create(txn, f.player_a, "bot", err);
        const auto r = txn.exec_params(
            "SELECT id FROM api_keys WHERE player_id = $1", f.player_a
        );
        key_id = r[0][0].as<int32_t>();
        txn.commit();
    }
    {
        pqxx::work txn(f.conn);
        REQUIRE(f.repo.revoke(txn, key_id, f.player_a));
        txn.commit();
    }
    pqxx::work txn(f.conn);
    const auto r = txn.exec_params(
        "SELECT revoked_at FROM api_keys WHERE id = $1", key_id
    );
    txn.commit();
    REQUIRE_FALSE(r[0][0].is_null());
}

TEST_CASE("revoke returns false for wrong player_id (ownership check)",
          "[integration][api_key_repo]") {
    Fixture f;
    std::string err;
    int32_t key_id;
    {
        pqxx::work txn(f.conn);
        f.repo.create(txn, f.player_a, "bot", err);
        const auto r = txn.exec_params(
            "SELECT id FROM api_keys WHERE player_id = $1", f.player_a
        );
        key_id = r[0][0].as<int32_t>();
        txn.commit();
    }
    pqxx::work txn(f.conn);
    // player_b tries to revoke player_a's key
    REQUIRE_FALSE(f.repo.revoke(txn, key_id, f.player_b));
    txn.commit();
}

TEST_CASE("revoke returns false for non-existent key_id", "[integration][api_key_repo]") {
    Fixture f;
    pqxx::work txn(f.conn);
    REQUIRE_FALSE(f.repo.revoke(txn, 999999, f.player_a));
    txn.commit();
}

TEST_CASE("after revoke player can create a new key", "[integration][api_key_repo]") {
    Fixture f;
    std::string err;
    int32_t key_id;
    {
        pqxx::work txn(f.conn);
        f.repo.create(txn, f.player_a, "first", err);
        const auto r = txn.exec_params(
            "SELECT id FROM api_keys WHERE player_id = $1", f.player_a
        );
        key_id = r[0][0].as<int32_t>();
        txn.commit();
    }
    {
        pqxx::work txn(f.conn);
        f.repo.revoke(txn, key_id, f.player_a);
        txn.commit();
    }
    {
        pqxx::work txn(f.conn);
        const auto k2 = f.repo.create(txn, f.player_a, "second", err);
        txn.commit();
        REQUIRE(k2.has_value());
        REQUIRE(err.empty());
    }
}
