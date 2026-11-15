#include "cs2kz_config.h"
#include <cassert>
#include <iostream>
int main() {
    KeyValues sqlite("ServerCfg");
    assert(sqlite.LoadFromBuffer("fixture",R"cfg(
      "ServerCfg" {
        // "apiUrl" "https://wrong.invalid"
        "apiUrl" "https://api.example.test/kz/"
        "replayDownloadUrl" "https://replays.example.test/{id}"
        "apiKey" "must-not-be-used"
        "db" { "driver" "SQLite" "database" "custom_records" }
        "other" { "apiUrl" "https://nested.invalid" }
      }
    )cfg"));
    auto s=line::readCS2KZConnections(sqlite);
    assert(s.api=="https://api.example.test/kz");assert(s.download=="https://replays.example.test/{id}");
    assert(s.driver=="sqlite"&&s.database=="addons/cs2kz/data/custom_records.sqlite3");
    KeyValues mysql("ServerCfg");
    assert(mysql.LoadFromBuffer("fixture",R"cfg("ServerCfg" {
      "db" { "driver" "mysql" "database" "kz" "host" "db.internal" "user" "reader"
             "pass" "literal//p%ss'word" "port" "3307" "timeout" "45" }
    })cfg"));
    auto m=line::readCS2KZConnections(mysql);
    assert(m.host=="db.internal"&&m.user=="reader"&&m.password=="literal//p%ss'word"&&m.database=="kz");
    assert(m.port==3307&&m.timeout==45&&m.api=="https://api.cs2kz.org");
    KeyValues disabled("ServerCfg");assert(disabled.LoadFromBuffer("fixture",R"("ServerCfg" { "apiUrl" "" "replayDownloadUrl" "" })"));
    auto d=line::readCS2KZConnections(disabled);assert(d.api.empty()&&d.download.empty()&&d.driver.empty());
    std::cout<<"CS2KZ original cfg: KeyValues, SQLite, MySQL, endpoints, defaults, disabled API and literal credentials: passed\n";
}
