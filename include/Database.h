#ifndef DATABASE_H
#define DATABASE_H

#include <string>
#include <vector>
#include <sqlite3.h>

#include "Expense.h"
#include "Income.h"
#include "Bill.h"

// SQLite-backed persistence layer for the Personal Budget System.
//
// Mirrors the in-memory state held by BudgetManager:
//   * users          — account credentials (SHA-256 hashed)
//   * expenses       — one row per Expense, foreign-keyed to users
//   * incomes        — one row per Income,  foreign-keyed to users
//   * bills          — one row per Bill,    foreign-keyed to users
//   * budget_limits  — per-category monthly budget caps, foreign-keyed to users
//
// Hydration pattern: on login, the caller drains the loadXxx() vectors into
// BudgetManager. On logout, the caller walks BudgetManager's getters and feeds
// each record back through insertXxx() / upsertBudgetLimit().
class Database {
public:
    struct BudgetLimitRow {
        std::string category;
        double      limit;
        int         month;
        int         year;
    };

    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    // ── Auth ──────────────────────────────────────────────────────────────
    // Returns the new user id on success, -1 if the username is taken.
    int registerUser(const std::string& username, const std::string& password);

    // Returns the user id if the credentials match, -1 otherwise.
    int loginUser(const std::string& username, const std::string& password);

    // ── Hydration (called once per login) ─────────────────────────────────
    std::vector<Expense>        loadExpenses    (int userId);
    std::vector<Income>         loadIncomes     (int userId);
    std::vector<Bill>           loadBills       (int userId);
    std::vector<BudgetLimitRow> loadBudgetLimits(int userId);

    // ── Mutations (write-through on every BudgetManager change) ──────────
    void insertExpense    (int userId, const Expense& e);
    void insertIncome     (int userId, const Income&  i);
    void insertBill       (int userId, const Bill&    b);
    void upsertBudgetLimit(int userId, const std::string& category,
                           double limit, int month, int year);

    // Clears all rows for a user, used right before re-dumping the full
    // BudgetManager state on logout. Keeps the on-disk view exactly in sync.
    void clearUserData(int userId);

private:
    sqlite3* db_;

    void               initSchema();
    static std::string hashPassword(const std::string& password);
};

#endif // DATABASE_H
