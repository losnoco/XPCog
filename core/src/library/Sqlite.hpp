// A thin RAII layer over the sqlite3 C API. Internal to the library code.
//
// Core talks to sqlite3 directly rather than through QtSql, because the scanner,
// the CLI and the tests all need the library without a QCoreApplication. Cog has
// the same precedent: Utils/SQLiteStore.m is 2,247 lines of hand-rolled sqlite3
// alongside Core Data.
//
// The point of this file is that no statement can leak and no error can be
// ignored silently -- both of which are easy with the raw API.

#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::sql {

class Statement;

class Database {
public:
    Database() = default;
    ~Database() { close(); }

    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    Database& operator=(Database&& other) noexcept {
        if (this != &other) {
            close();
            handle_        = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    /// Opens or creates the database. `:memory:` is accepted for tests.
    [[nodiscard]] bool open(const std::filesystem::path& path);
    void               close();

    [[nodiscard]] bool isOpen() const noexcept { return handle_ != nullptr; }

    /// Runs one or more statements, discarding any rows.
    [[nodiscard]] bool exec(std::string_view sql);

    [[nodiscard]] std::int64_t lastInsertRowId() const;
    [[nodiscard]] std::string  lastError() const;

    [[nodiscard]] sqlite3* handle() const noexcept { return handle_; }

private:
    sqlite3* handle_ = nullptr;
};

/// One prepared statement. Binding is 1-based, as in sqlite3; reading is 0-based,
/// also as in sqlite3. Keeping both conventions avoids a translation that would
/// silently be off by one against the documentation.
class Statement {
public:
    Statement(Database& database, std::string_view sql);
    ~Statement();

    Statement(const Statement&)            = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] bool valid() const noexcept { return statement_ != nullptr; }

    void bind(int index, std::int64_t value);
    void bind(int index, double value);
    void bind(int index, std::string_view value);
    void bind(int index, std::span<const std::byte> value);
    void bindNull(int index);

    /// Advances to the next row. Returns true while rows remain.
    [[nodiscard]] bool step();

    /// Runs a statement expected to produce no rows.
    [[nodiscard]] bool run();

    void reset();

    [[nodiscard]] bool                   isNull(int column) const;
    [[nodiscard]] std::int64_t           columnInt(int column) const;
    [[nodiscard]] double                 columnDouble(int column) const;
    [[nodiscard]] std::string            columnText(int column) const;
    [[nodiscard]] std::vector<std::byte> columnBlob(int column) const;

private:
    Database*     database_  = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

/// Commits on success, rolls back if it goes out of scope without a commit.
/// The 50k-entry insert budget is only reachable inside one of these: sqlite
/// otherwise gives every statement its own transaction and its own fsync.
class Transaction {
public:
    explicit Transaction(Database& database);
    ~Transaction();

    Transaction(const Transaction&)            = delete;
    Transaction& operator=(const Transaction&) = delete;

    [[nodiscard]] bool commit();

private:
    Database* database_ = nullptr;
    bool      active_   = false;
};

}  // namespace xpcog::sql
