#pragma once
#include <keyvalues.h>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace line {
struct CS2KZConnections {
    std::string api,download,driver,database,host,user,password;
    int port=3306,timeout=10;
};
// Use the timer's own KeyValues syntax and field names, including its SQLite base path.
// Copy all strings before the timer config tree is freed; API credentials are not needed for public GETs.
inline CS2KZConnections readCS2KZConnections(KeyValues& root) {
    CS2KZConnections c;
    c.api=root.GetString("apiUrl","https://api.cs2kz.org");
    while(!c.api.empty()&&c.api.back()=='/') c.api.pop_back();
    c.download=root.GetString("replayDownloadUrl","https://replays.cs2kz.org/{id}");
    auto http=[](const std::string& url){return url.rfind("https://",0)==0||url.rfind("http://",0)==0;};
    if(!c.api.empty()&&!http(c.api)) throw std::runtime_error("Invalid apiUrl in CS2KZ config.");
    if(!c.download.empty()&&(!http(c.download)||c.download.find("{id}")==std::string::npos))
        throw std::runtime_error("Invalid replayDownloadUrl in CS2KZ config; expected an HTTP URL with {id}.");
    if(auto* db=root.FindKey("db")) {
        c.driver=db->GetString("driver");c.database=db->GetString("database");
        std::transform(c.driver.begin(),c.driver.end(),c.driver.begin(),[](unsigned char v){return char(std::tolower(v));});
        if(c.driver=="sqlite") c.database="addons/cs2kz/data/"+c.database+".sqlite3";
        c.host=db->GetString("host");c.user=db->GetString("user");c.password=db->GetString("pass");
        c.port=db->GetInt("port",3306);c.timeout=db->GetInt("timeout",10);
    }
    return c;
}
} // namespace line
