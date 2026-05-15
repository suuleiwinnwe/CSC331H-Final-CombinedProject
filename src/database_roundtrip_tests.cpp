// database_roundtrip_tests.cpp
// ----------------------------------------------------------------------------
// Verifies that EVERY field that BudgetManager hands to the Database actually
// makes it to disk and comes back identical. If any field is silently dropped
// or transformed, this test fails.
//
// Coverage map — what BudgetManager stores vs. what we verify here:
//
//   Expense:    date, amount, category, description, payment method     ✓
//   Income:     date, amount, category, description, source             ✓
//   Bill:       name, amount, due date, paid status                     ✓
//   Budget:     category, limit, month, year                            ✓
//   User:       username, password (hashed)                              ✓
//
// What deliberately does NOT round-trip (in-memory only, by design):
//   * Transaction IDs (BudgetManager regenerates them on hydration)
//   * Category "totalSpent" (recomputed by walking expenses)
//   * Bill's in-memory billId (regenerated on hydration)
// ----------------------------------------------------------------------------
#include "Database.h"
#include "Date.h"
#include "Expense.h"
#include "Income.h"
#include "Bill.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <algorithm>
#include <cmath>

// ── Tiny test framework ──────────────────────────────────────────────────────
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* label) {
    if (cond) { ++g_pass; std::printf("  [PASS] %s\n", label); }
    else      { ++g_fail; std::printf("  [FAIL] %s\n", label); }
}
static void section(const char* name) { std::printf("\n== %s ==\n", name); }

static bool eqDouble(double a, double b) { return std::fabs(a - b) < 1e-6; }
static bool eqDate  (const Date& a, const Date& b) {
    return a.year == b.year && a.month == b.month && a.day == b.day;
}

// ────────────────────────────────────────────────────────────────────────────
int main() {
    const char* DB = "test_roundtrip.db";
    unlink(DB);
    unlink((std::string(DB) + "-wal").c_str());
    unlink((std::string(DB) + "-shm").c_str());

    Database db(DB);
    int uid = db.registerUser("roundtrip_user", "pw");
    if (uid <= 0) { std::printf("FATAL: could not create test user\n"); return 1; }

    // ============================================================
    section("Expenses — every field round-trips");
    // ============================================================
    {
        // Deliberately varied: different categories, dates spanning a year,
        // amounts including cents, descriptions with spaces/punctuation, all
        // payment-method strings the GUI offers, plus an empty-description case.
        Expense inputs[] = {
            Expense(0, Date(2026, 1,  3),  12.50, "Food",          "Coffee & bagel",         "Card"),
            Expense(0, Date(2026, 2, 14),  78.99, "Entertainment", "Valentine's dinner",     "Card"),
            Expense(0, Date(2026, 3, 30),  45.00, "Transportation","Bus pass (monthly)",     "Cash"),
            Expense(0, Date(2026, 5, 12), 199.95, "Shopping",      "New headphones",         "Bank Transfer"),
            Expense(0, Date(2026, 7,  1),   3.75, "Food",          "",                       ""),         // empty desc + method
            Expense(0, Date(2026,11,28),  240.00, "Health",        "Dental cleaning",        "Card"),
            Expense(0, Date(2025,12, 31),   9.99, "Utilities",     "Streaming subscription", "Card"),
        };
        const int N = sizeof(inputs)/sizeof(inputs[0]);

        for (auto& e : inputs) db.insertExpense(uid, e);
        auto loaded = db.loadExpenses(uid);

        check((int)loaded.size() == N, "all expenses returned by loadExpenses");
        if ((int)loaded.size() == N) {
            // Match input[i] against any loaded row (order is technically
            // ORDER BY id, but we don't rely on that). For each input field,
            // require that AT LEAST one loaded row matches every field.
            for (int i = 0; i < N; ++i) {
                const Expense& want = inputs[i];
                bool found = false;
                for (const Expense& got : loaded) {
                    if (eqDate(want.getDate(), got.getDate())
                        && eqDouble(want.getAmount(), got.getAmount())
                        && want.getCategory()      == got.getCategory()
                        && want.getDescription()   == got.getDescription()
                        && want.getPaymentMethod() == got.getPaymentMethod())
                    { found = true; break; }
                }
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "expense[%d] all fields round-trip (%s / $%.2f / %s)",
                              i, want.getCategory().c_str(), want.getAmount(),
                              want.getDescription().empty() ? "<empty>"
                                                            : want.getDescription().c_str());
                check(found, buf);
            }
        }
    }

    // ============================================================
    section("Incomes — every field round-trips (incl. source)");
    // ============================================================
    db.clearUserData(uid);
    {
        Income inputs[] = {
            Income(0, Date(2026, 1, 15), 3200.00, "Salary",   "Biweekly paycheck", "Acme Corp"),
            Income(0, Date(2026, 2, 15), 3200.00, "Salary",   "Biweekly paycheck", "Acme Corp"),
            Income(0, Date(2026, 4,  3),  450.00, "Side Gig", "Logo design",       "Freelance"),
            Income(0, Date(2026, 6, 20),   75.00, "Gift",     "",                  "Mom"),  // empty desc
            Income(0, Date(2026, 9,  1),   12.34, "Other",    "Rebate refund",     ""),     // empty source
        };
        const int N = sizeof(inputs)/sizeof(inputs[0]);

        for (auto& i : inputs) db.insertIncome(uid, i);
        auto loaded = db.loadIncomes(uid);

        check((int)loaded.size() == N, "all incomes returned by loadIncomes");
        if ((int)loaded.size() == N) {
            for (int i = 0; i < N; ++i) {
                const Income& want = inputs[i];
                bool found = false;
                for (const Income& got : loaded) {
                    if (eqDate(want.getDate(), got.getDate())
                        && eqDouble(want.getAmount(), got.getAmount())
                        && want.getCategory()    == got.getCategory()
                        && want.getDescription() == got.getDescription()
                        && want.getSource()      == got.getSource())
                    { found = true; break; }
                }
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "income[%d] all fields round-trip ($%.2f / %s / src=%s)",
                              i, want.getAmount(), want.getCategory().c_str(),
                              want.getSource().empty() ? "<empty>" : want.getSource().c_str());
                check(found, buf);
            }
        }
    }

    // ============================================================
    section("Bills — every field round-trips (incl. paid status)");
    // ============================================================
    db.clearUserData(uid);
    {
        Bill inputs[] = {
            Bill(0, "Rent",          1200.00, Date(2026, 6,  1), false),
            Bill(0, "Internet",         60.00, Date(2026, 5,  5), true),   // paid
            Bill(0, "Electricity",      85.43, Date(2026, 5, 15), false),
            Bill(0, "Phone",            40.00, Date(2026, 5, 20), false),
            Bill(0, "Water",            30.00, Date(2026, 4, 30), true),   // paid + overdue
            Bill(0, "Gym Membership",   29.99, Date(2026, 5, 12), false),  // due today (relative)
        };
        const int N = sizeof(inputs)/sizeof(inputs[0]);

        for (auto& b : inputs) db.insertBill(uid, b);
        auto loaded = db.loadBills(uid);

        check((int)loaded.size() == N, "all bills returned by loadBills");
        if ((int)loaded.size() == N) {
            for (int i = 0; i < N; ++i) {
                const Bill& want = inputs[i];
                bool found = false;
                for (const Bill& got : loaded) {
                    if (want.getBillName()    == got.getBillName()
                        && eqDouble(want.getAmount(), got.getAmount())
                        && eqDate(want.getDueDate(), got.getDueDate())
                        && want.getIsPaid()   == got.getIsPaid())
                    { found = true; break; }
                }
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "bill[%d] all fields round-trip (%s / $%.2f / %s)",
                              i, want.getBillName().c_str(), want.getAmount(),
                              want.getIsPaid() ? "paid" : "unpaid");
                check(found, buf);
            }
        }
    }

    // ============================================================
    section("Budget limits — every field round-trips, scoped per month");
    // ============================================================
    db.clearUserData(uid);
    {
        struct Want { std::string cat; double limit; int month; int year; };
        Want inputs[] = {
            {"Food",           500.00, 5, 2026},
            {"Food",           550.00, 6, 2026},   // same category, different month
            {"Utilities",      200.00, 5, 2026},
            {"Entertainment",  150.00, 5, 2026},
            {"Transportation",  80.00, 5, 2026},
            {"Health",         300.00, 5, 2026},
            {"Shopping",       250.00, 5, 2026},
            {"Other",          100.00, 5, 2026},
        };
        const int N = sizeof(inputs)/sizeof(inputs[0]);

        for (const auto& w : inputs) db.upsertBudgetLimit(uid, w.cat, w.limit, w.month, w.year);
        auto loaded = db.loadBudgetLimits(uid);

        check((int)loaded.size() == N,
              "all budget-limit rows returned by loadBudgetLimits (incl. same-category-different-month)");

        if ((int)loaded.size() == N) {
            for (int i = 0; i < N; ++i) {
                const auto& want = inputs[i];
                bool found = false;
                for (const auto& got : loaded) {
                    if (want.cat   == got.category
                        && eqDouble(want.limit, got.limit)
                        && want.month == got.month
                        && want.year  == got.year) { found = true; break; }
                }
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "budget[%d] all fields round-trip (%s / $%.2f / %d-%d)",
                              i, want.cat.c_str(), want.limit, want.year, want.month);
                check(found, buf);
            }
        }
    }

    // ============================================================
    section("User credentials — username persisted, password hashed (not plaintext)");
    // ============================================================
    {
        // Verify that the password we registered is NOT stored as plain text.
        // Open the SQLite file directly with the sqlite3 CLI through a system
        // call, and look for the original password string. We expect it to be
        // absent — only the SHA-256 hash should be on disk.
        std::string cmd = std::string("sqlite3 ") + DB +
                          " \"SELECT password_hash FROM users WHERE username='roundtrip_user';\"";
        FILE* p = popen(cmd.c_str(), "r");
        char hashBuf[256] = {0};
        if (p) { std::fgets(hashBuf, sizeof(hashBuf), p); pclose(p); }
        std::string hash(hashBuf);
        while (!hash.empty() && (hash.back() == '\n' || hash.back() == '\r')) hash.pop_back();

        check(!hash.empty(),
              "password_hash row exists for registered user");
        check(hash != "pw",
              "stored password is NOT the plain-text value");
        check(hash.length() == 64,
              "stored password is a 64-char SHA-256 hex digest");
        check(hash.find_first_not_of("0123456789abcdef") == std::string::npos,
              "stored password is valid lowercase hex");
        check(db.loginUser("roundtrip_user", "pw") == uid,
              "login still succeeds with the original plain-text password");
    }

    // ============================================================
    section("Full snapshot: realistic mixed workload survives clear + reload");
    // ============================================================
    db.clearUserData(uid);
    {
        // Build a realistic month of activity, write it, then read it back
        // and confirm every category of record came through.
        db.upsertBudgetLimit(uid, "Food", 500.0, 5, 2026);
        db.upsertBudgetLimit(uid, "Utilities", 200.0, 5, 2026);

        db.insertExpense(uid, Expense(0, Date(2026,5, 1),  85.00, "Food",      "Groceries",  "Card"));
        db.insertExpense(uid, Expense(0, Date(2026,5, 8),  90.00, "Food",      "Groceries",  "Card"));
        db.insertExpense(uid, Expense(0, Date(2026,5,15),  75.00, "Utilities", "Electric",   "Bank Transfer"));

        db.insertIncome (uid, Income (0, Date(2026,5, 1), 1600.00, "Salary", "Paycheck", "Acme"));

        db.insertBill   (uid, Bill   (0, "Rent",     1200.00, Date(2026,6, 1), false));
        db.insertBill   (uid, Bill   (0, "Internet",   60.00, Date(2026,5, 5), true));

        // Hydration check: every category of data should be present.
        check(db.loadExpenses    (uid).size() == 3, "snapshot expenses present");
        check(db.loadIncomes     (uid).size() == 1, "snapshot incomes present");
        check(db.loadBills       (uid).size() == 2, "snapshot bills present");
        check(db.loadBudgetLimits(uid).size() == 2, "snapshot budgets present");

        // Now wipe the user and confirm everything is gone.
        db.clearUserData(uid);
        check(db.loadExpenses    (uid).empty(), "clearUserData wipes expenses");
        check(db.loadIncomes     (uid).empty(), "clearUserData wipes incomes");
        check(db.loadBills       (uid).empty(), "clearUserData wipes bills");
        check(db.loadBudgetLimits(uid).empty(), "clearUserData wipes budgets");

        // The user row itself must NOT be wiped by clearUserData.
        check(db.loginUser("roundtrip_user", "pw") == uid,
              "clearUserData preserves the user account itself");
    }

    // ── Cleanup & summary ───────────────────────────────────────────────────
    unlink(DB);
    unlink((std::string(DB) + "-wal").c_str());
    unlink((std::string(DB) + "-shm").c_str());

    std::printf("\n----------------------------------------\n");
    std::printf("Passed: %d   Failed: %d\n", g_pass, g_fail);
    std::printf("----------------------------------------\n");
    return g_fail == 0 ? 0 : 1;
}
