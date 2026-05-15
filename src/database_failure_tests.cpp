// database_failure_tests.cpp
// ----------------------------------------------------------------------------
// Exercises Database failure modes. Does NOT cover happy paths.
//
// What we want to know:
//   * Are invalid inputs rejected cleanly (return -1, no crash, no data leak)?
//   * Are SQL-injection payloads neutralised by parameter binding?
//   * Do foreign-key violations behave sanely?
//   * Does the class refuse to construct against an unwritable path?
//   * Does double-registering the same user fail without corrupting anything?
//   * Are loads against bogus user IDs empty rather than throwing?
//   * Does clearUserData on a non-existent user behave?
//
// Build: see Makefile target `test-db`.
// ----------------------------------------------------------------------------
#include "Database.h"
#include "Date.h"
#include "Expense.h"
#include "Income.h"
#include "Bill.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

// ── Tiny test framework ──────────────────────────────────────────────────────
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* label) {
    if (cond) {
        ++g_pass;
        std::printf("  [PASS] %s\n", label);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s\n", label);
    }
}

static void section(const char* name) {
    std::printf("\n== %s ==\n", name);
}

// Convenience: drop the test database between groups so each section starts clean.
static void resetDb(const char* path) {
    unlink(path);
}

// ────────────────────────────────────────────────────────────────────────────
int main() {
    const char* DB = "test_failures.db";

    // -----------------------------------------------------------------------
    section("Construction against an unwritable path");
    // -----------------------------------------------------------------------
    {
        bool threw = false;
        try {
            // /no_such_dir cannot be created by an unprivileged process on macOS.
            Database d("/no_such_dir_xyz/forbidden.db");
            (void)d;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "construct against unwritable path throws std::runtime_error");
    }

    // -----------------------------------------------------------------------
    section("Register: duplicate username is rejected");
    // -----------------------------------------------------------------------
    resetDb(DB);
    {
        Database db(DB);
        int u1 = db.registerUser("alice", "pw1");
        int u2 = db.registerUser("alice", "pw2");  // collision
        check(u1 > 0,             "first registration returns positive id");
        check(u2 == -1,           "duplicate username returns -1");
        check(u1 != u2,           "ids differ between accepted and rejected");
    }

    // -----------------------------------------------------------------------
    section("Login: wrong-password / missing-user / empty-creds all fail");
    // -----------------------------------------------------------------------
    resetDb(DB);
    {
        Database db(DB);
        int alice = db.registerUser("alice", "correct horse battery staple");

        check(db.loginUser("alice", "")             == -1, "empty password rejected");
        check(db.loginUser("alice", "wrong")        == -1, "wrong password rejected");
        check(db.loginUser("ALICE", "correct horse battery staple") == -1,
              "username is case-sensitive (ALICE != alice)");
        check(db.loginUser("nobody", "anything")    == -1, "unknown username rejected");
        check(db.loginUser("", "")                  == -1, "empty username rejected");
        check(db.loginUser("alice", "correct horse battery staple") == alice,
              "(sanity) correct credentials still succeed");
    }

    // -----------------------------------------------------------------------
    section("SQL injection — parameterised binding must neutralise payloads");
    // -----------------------------------------------------------------------
    resetDb(DB);
    {
        Database db(DB);
        int victim = db.registerUser("victim", "secret");
        check(victim > 0, "(setup) victim account created");

        // Classic payloads that would break a naïve string-concatenated query.
        const char* payloads[] = {
            "victim'--",
            "victim' OR '1'='1",
            "'; DROP TABLE users; --",
            "victim\"; DELETE FROM users;--",
            "victim' UNION SELECT 1,'x',''--",
        };
        for (const char* p : payloads) {
            int r = db.loginUser(p, "anything");
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "injection payload rejected: %s", p);
            check(r == -1, buf);
        }

        // After all that, the victim account must still log in normally
        // and the users table must still exist (i.e., DROP TABLE was inert).
        check(db.loginUser("victim", "secret") == victim,
              "victim account survives injection attempts");
    }

    // -----------------------------------------------------------------------
    section("Hydration against bogus user IDs returns empty (not crash)");
    // -----------------------------------------------------------------------
    resetDb(DB);
    {
        Database db(DB);
        check(db.loadExpenses    (-1).empty(), "loadExpenses(-1) empty");
        check(db.loadExpenses    (99999).empty(), "loadExpenses(99999) empty");
        check(db.loadIncomes     (-1).empty(), "loadIncomes(-1) empty");
        check(db.loadBills       (0).empty(),  "loadBills(0) empty");
        check(db.loadBudgetLimits(-1).empty(), "loadBudgetLimits(-1) empty");
    }

    // -----------------------------------------------------------------------
    section("Inserts with a valid user persist; with bogus user are noise but safe");
    // -----------------------------------------------------------------------
    resetDb(DB);
    {
        Database db(DB);
        int uid = db.registerUser("bob", "pw");
        check(uid > 0, "(setup) bob created");

        // Valid insert — sanity check the persistence round-trip
        Expense e(0, Date(2026, 5, 12), 12.50, "Food", "lunch", "Card");
        db.insertExpense(uid, e);
        auto loaded = db.loadExpenses(uid);
        check(loaded.size() == 1, "valid expense round-trips through SQLite");

        // FK is currently informational (we don't ON DELETE CASCADE), but
        // inserting against a non-existent user must not throw and must not
        // pollute the valid user's data.
        Expense ghost(0, Date(2026, 5, 12), 999.99, "Food", "ghost", "Cash");
        db.insertExpense(7777, ghost);
        auto loadedAfter = db.loadExpenses(uid);
        check(loadedAfter.size() == 1,
              "insertExpense against bogus user does not corrupt valid user's data");
    }

    // -----------------------------------------------------------------------
    section("upsertBudgetLimit: same (user, category, month, year) overwrites");
    // -----------------------------------------------------------------------
    resetDb(DB);
    {
        Database db(DB);
        int uid = db.registerUser("carol", "pw");
        db.upsertBudgetLimit(uid, "Food", 200.0, 5, 2026);
        db.upsertBudgetLimit(uid, "Food", 250.0, 5, 2026);  // should update, not duplicate
        auto rows = db.loadBudgetLimits(uid);
        check(rows.size() == 1, "duplicate upsert collapses to one row");
        check(rows.size() == 1 && rows[0].limit == 250.0,
              "duplicate upsert keeps the latest limit");
    }

    // -----------------------------------------------------------------------
    section("clearUserData: idempotent, scoped to the right user");
    // -----------------------------------------------------------------------
    resetDb(DB);
    {
        Database db(DB);
        int a = db.registerUser("alice2", "pw");
        int b = db.registerUser("bob2",   "pw");
        db.insertExpense(a, Expense(0, Date(2026,5,12), 10, "Food", "a", "Card"));
        db.insertExpense(b, Expense(0, Date(2026,5,12), 20, "Food", "b", "Card"));

        // Clearing user a must not touch user b
        db.clearUserData(a);
        check(db.loadExpenses(a).empty(),       "clearUserData wipes target user");
        check(db.loadExpenses(b).size() == 1,   "clearUserData leaves other users intact");

        // Re-clearing the same user is harmless
        db.clearUserData(a);
        check(db.loadExpenses(a).empty(),       "clearUserData is idempotent");

        // Clearing a non-existent user is harmless
        db.clearUserData(99999);
        check(db.loadExpenses(b).size() == 1,   "clearUserData on bogus user does nothing");
    }

    // -----------------------------------------------------------------------
    section("Long / unicode / unusual-but-legal input handled");
    // -----------------------------------------------------------------------
    resetDb(DB);
    {
        Database db(DB);
        // Very long username (1 KB) — SQLite TEXT has no fixed limit, so this
        // should round-trip.
        std::string longName(1024, 'x');
        int u = db.registerUser(longName, "pw");
        check(u > 0,                                  "1KB username accepted");
        check(db.loginUser(longName, "pw") == u,      "1KB username can log in");
        check(db.loginUser(longName, "no")  == -1,    "1KB username + wrong pw rejected");

        // Unicode + emoji round-trip
        std::string unicodeUser = u8"用户_тест_🚀";
        int uu = db.registerUser(unicodeUser, "p");
        check(uu > 0,                                 "unicode/emoji username accepted");
        check(db.loginUser(unicodeUser, "p") == uu,   "unicode/emoji username logs in");
    }

    // -----------------------------------------------------------------------
    section("Concurrent open of the same DB file (WAL should allow this)");
    // -----------------------------------------------------------------------
    resetDb(DB);
    {
        Database d1(DB);
        bool ok = false;
        try {
            Database d2(DB);
            int u = d2.registerUser("dual", "pw");
            ok = (u > 0);
        } catch (const std::runtime_error&) {
            ok = false;
        }
        check(ok, "second Database handle on same file can open and write (WAL mode)");
    }

    // -----------------------------------------------------------------------
    section("loadBills returns paid status faithfully");
    // -----------------------------------------------------------------------
    resetDb(DB);
    {
        Database db(DB);
        int uid = db.registerUser("eve", "pw");
        Bill unpaid(0, "Rent",     1200.0, Date(2026, 6, 1), false);
        Bill paid  (0, "Internet",   60.0, Date(2026, 5, 5), true);
        db.insertBill(uid, unpaid);
        db.insertBill(uid, paid);
        auto bills = db.loadBills(uid);
        check(bills.size() == 2,                              "both bills loaded");
        bool sawUnpaid = false, sawPaid = false;
        for (const auto& b : bills) {
            if (b.getBillName() == "Rent"     && !b.getIsPaid()) sawUnpaid = true;
            if (b.getBillName() == "Internet" &&  b.getIsPaid()) sawPaid   = true;
        }
        check(sawUnpaid && sawPaid, "is_paid round-trips correctly through SQLite");
    }

    // -----------------------------------------------------------------------
    section("Empty-string fields do not crash and round-trip as empty");
    // -----------------------------------------------------------------------
    resetDb(DB);
    {
        Database db(DB);
        int uid = db.registerUser("frank", "pw");
        Expense e(0, Date(2026,5,12), 1.0, "Other", "", "");  // empty desc + method
        db.insertExpense(uid, e);
        auto rows = db.loadExpenses(uid);
        check(rows.size() == 1 && rows[0].getDescription().empty() &&
              rows[0].getPaymentMethod().empty(),
              "empty description and payment method round-trip");
    }

    // -----------------------------------------------------------------------
    section("Re-registering an empty-string username — does SQLite UNIQUE catch it?");
    // -----------------------------------------------------------------------
    resetDb(DB);
    {
        Database db(DB);
        int a = db.registerUser("", "p1");
        int b = db.registerUser("", "p2");
        // SQLite considers '' a valid unique value, so the first insert
        // succeeds; the second collides. Document the behaviour either way.
        std::printf("    (note) first empty-username register => %d\n", a);
        std::printf("    (note) second empty-username register => %d\n", b);
        check(!(a > 0 && b > 0), "two empty-string usernames cannot both succeed");
    }

    // ── Summary ─────────────────────────────────────────────────────────────
    std::printf("\n----------------------------------------\n");
    std::printf("Passed: %d   Failed: %d\n", g_pass, g_fail);
    std::printf("----------------------------------------\n");
    unlink(DB);
    unlink((std::string(DB) + "-wal").c_str());
    unlink((std::string(DB) + "-shm").c_str());
    return g_fail == 0 ? 0 : 1;
}
