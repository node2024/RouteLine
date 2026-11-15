#pragma once
#include "config.h"
#include "cs2kz_config.h"
#include <filesystem/ifilesystem.h>
#include <keyvalues.h>
#include <steam/steam_gameserver.h>
#include <steam/steam_api_flat.h>
#include <vector>
#include <sql_mm.h>
#include <mysql_mm.h>
#include <sqlite_mm.h>
#include <chrono>
#include <deque>
#include <filesystem>

namespace line {
// SQLMM and Steam HTTP APIs are polled on the server thread. File decoding happens elsewhere.
class CS2KZProvider final:public IRouteProvider {
    using Clock=std::chrono::steady_clock;
    using HttpCallback=std::function<void(std::string,std::string)>;
    struct HTTP { HTTPRequestHandle handle=0; SteamAPICall_t call=0; Clock::time_point start; size_t limit=0; int timeout=0; HttpCallback callback; };
    std::string base_,api_,download_,driver_,database_,host_,user_,password_;
    int port_=3306,timeout_=10;
    ISQLInterface* sql_=nullptr;
    ISQLConnection* connection_=nullptr;
    bool connecting_=false,connected_=false,failed_=false;
    size_t pendingSQL_=0;
    uint64_t connectionGeneration_=0;
    Clock::time_point reconnect_{};
    std::vector<HTTP> http_;
    std::deque<std::function<void()>> ready_;
    size_t fileLimit_=128*1024*1024;

    void get(std::string url,size_t limit,int timeout,HttpCallback callback) {
        auto* h=SteamAPI_SteamGameServerHTTP();
        if(!h||http_.size()>=8) { ready_.push_back([callback]{callback({},"Global HTTP is unavailable or busy; try again shortly.");});return; }
        HTTP req;req.start=Clock::now();req.limit=limit;req.timeout=timeout;req.callback=callback;
        req.handle=h->CreateHTTPRequest(k_EHTTPMethodGET,url.c_str());
        bool sent=false;
        if(req.handle) {
            h->SetHTTPRequestAbsoluteTimeoutMS(req.handle,timeout*1000);
            h->SetHTTPRequestRequiresVerifiedCertificate(req.handle,true);
            h->SetHTTPRequestUserAgentInfo(req.handle,"RouteLine/0.16.1");
            sent=h->SendHTTPRequest(req.handle,&req.call);
        }
        if(!sent) {
            if(req.handle) h->ReleaseHTTPRequest(req.handle);
            ready_.push_back([callback]{callback({},"Could not send Global HTTP request.");});return;
        }
        http_.push_back(std::move(req));
    }
    void localQuery(const std::string& query,std::function<void(ISQLResult*,std::string)> callback) {
        ++pendingSQL_;const auto generation=connectionGeneration_;
        connection_->ExecuteTransaction({{query}},[this,callback,generation](std::vector<ISQLQuery*> queries) {
            if(generation!=connectionGeneration_) return;
            --pendingSQL_;
            callback(queries.empty()?nullptr:queries[0]->GetResultSet(),{});
        },[this,callback,generation](std::string,int) {
            if(generation!=connectionGeneration_) return;
            --pendingSQL_;callback(nullptr,"SR database query failed. Check the CS2KZ schema and connection.");
        });
    }
    void load(Query q,Record record,bool available,BytesCallback callback) {
        if(!uuid(record.id)||!std::isfinite(record.time)||record.time<=0||record.teleports<0||(q.pro&&record.teleports)) {
            callback(q,{}, {},"Invalid record received from the timer backend.");return;
        }
        if(isCached&&isCached(q,record)) {callback(q,record,{},{});return;}
        for(const auto& sub:{"/kzreplays/","/kzreplays/downloads/"}) {
            auto path=base_+sub+record.id+".replay";std::error_code error;
            if(std::filesystem::is_regular_file(path,error)) { callback(q,record,{path,{},decodeReplay},{});return; }
        }
        if(!q.global||!available) {
            const auto source=q.global?"Global #":"SR #";
            callback(q,{}, {},source+std::to_string(q.rank)+" has no available replay.");return;
        }
        if(download_.empty()) {callback(q,{}, {},"Global replay downloads are disabled in the CS2KZ configuration.");return;}
        auto url=download_;auto pos=url.find("{id}");url.replace(pos,4,record.id);
        get(url,fileLimit_,45,[q,record,callback](std::string bytes,std::string error) {
            callback(q,record,{{},std::move(bytes),decodeReplay},std::move(error));
        });
    }
    void select(Query q,BytesCallback callback) {
        if(q.global) {
            get(recordsURL(api_,q),1024*1024,12,[this,q,callback](std::string body,std::string error) {
                if(!error.empty()) {callback(q,{}, {},error);return;}
                try {
                    const auto data=Json::parse(body);const auto& values=data.at("values");
                    if(!values.is_array()) throw std::runtime_error("values");
                    if(values.empty()) {callback(q,{}, {},"No Global #"+std::to_string(q.rank)+" for this map/course/mode/category.");return;}
                    const auto& row=values.at(0);
                    if(row.at("map").at("name")!=q.map||row.at("course").at("name")!=q.course||row.at("mode")!=q.mode
                       ||!row.at("styles").empty()) throw std::runtime_error("query mismatch");
                    Record record{row.at("id").get<std::string>(),row.at("player").at("name").get<std::string>(),row.at("time").get<double>(),row.at("teleports").get<int>()};
                    load(q,record,row.value("replay_available",false),callback);
                } catch(const std::exception&) {callback(q,{}, {},"Invalid Global record response.");}
            });
        } else {
            auto query=recordSQL(q,[this](const std::string& text){return connection_->Escape(text.c_str());});
            localQuery(query,[this,q,callback](ISQLResult* result,std::string error) {
                if(!error.empty()) {ready_.push_back([q,callback,error]{callback(q,{}, {},error);});return;}
                if(!result||!result->FetchRow()) {ready_.push_back([q,callback]{callback(q,{}, {},"No SR #"+std::to_string(q.rank)+" for this map/course/mode/category.");});return;}
                try {
                    Record record{result->GetString(0),result->GetString(1),std::stod(result->GetString(2)),result->GetInt(3)};
                    ready_.push_back([this,q,record,callback]{load(q,record,true,callback);});
                } catch(...) {ready_.push_back([q,callback]{callback(q,{}, {},"Invalid SR database row.");});}
            });
        }
    }
public:
    std::function<bool(const Query&,const Record&)> isCached;
    void configure(const std::string& base,IFileSystem* fs,size_t fileLimit) {
        KeyValues root("ServerCfg");
        if(!root.LoadFromFile(fs,(base+"/cfg/cs2kz-server-config.txt").c_str(),nullptr))
            throw std::runtime_error("Cannot read cfg/cs2kz-server-config.txt.");
        const auto c=readCS2KZConnections(root);base_=base;api_=c.api;download_=c.download;fileLimit_=fileLimit;
        driver_=c.driver;database_=c.database;host_=c.host;user_=c.user;password_=c.password;port_=c.port;timeout_=c.timeout;
    }
    void refreshSQL(ISQLInterface* sql) {
        if(sql!=sql_) {
            // A removed provider owns the old connection. Never dereference it after its unload.
            sql_=sql;connection_=nullptr;connected_=connecting_=failed_=false;pendingSQL_=0;++connectionGeneration_;
        }
        if(!sql_||connection_||Clock::now()<reconnect_) return;
        reconnect_=Clock::now()+std::chrono::seconds(5);
        if(driver_=="sqlite") {
            std::error_code error;
            if(!std::filesystem::is_regular_file(std::filesystem::path(base_)/database_,error)) return;
            auto* client=sql_->GetSQLiteClient();if(client) connection_=client->CreateSQLiteConnection({database_.c_str()});
        } else if(driver_=="mysql") {
            auto* client=sql_->GetMySQLClient();
            if(client) connection_=client->CreateMySQLConnection({host_.c_str(),user_.c_str(),password_.c_str(),database_.c_str(),port_,timeout_});
        }
        if(connection_) {
            connecting_=true;const auto generation=connectionGeneration_;
            connection_->Connect([this,generation](bool ok) {
                if(generation!=connectionGeneration_) return;
                connecting_=false;connected_=ok;failed_=!ok;
            });
        }
    }
    bool busySQL() const {return pendingSQL_||connecting_;}
    void request(const Query& q,BytesCallback callback) override {
        if(q.global&&api_.empty()) {callback(q,{}, {},"Global API is disabled in the CS2KZ configuration.");return;}
        if(!q.global&&(!connected_||!connection_||pendingSQL_>=8)) {
            callback(q,{}, {},"SR database is unavailable or busy. Check SQLMM and CS2KZ database settings.");return;
        }
        if(!q.course.empty()) {select(q,callback);return;}
        // No active timer course: use the first configured course, preserving its exact case.
        if(q.global) {
            get(api_+"/maps/"+encode(q.map),1024*1024,12,[this,q=Query(q),callback](std::string body,std::string error) mutable {
                if(!error.empty()) {callback(q,{}, {},error);return;}
                try {
                    const auto data=Json::parse(body);const auto& courses=data.at("courses");
                    if(courses.empty()) {callback(q,{}, {},"This map has no Global courses.");return;}
                    q.course=courses.at(0).at("name").get<std::string>();select(q,callback);
                }catch(...) {callback(q,{}, {},"Invalid Global map response.");}
            });
        } else {
            auto query="SELECT mc.Name FROM MapCourses mc JOIN Maps m ON m.ID=mc.MapID WHERE m.Name='"+connection_->Escape(q.map.c_str())+"' ORDER BY mc.StageID,mc.ID LIMIT 1";
            localQuery(query,[this,q=Query(q),callback](ISQLResult* result,std::string error) mutable {
                if(!error.empty()) {ready_.push_back([q,callback,error]{callback(q,{}, {},error);});return;}
                if(!result||!result->FetchRow()) {ready_.push_back([q,callback]{callback(q,{}, {},"No local courses found for this map.");});return;}
                q.course=result->GetString(0);ready_.push_back([this,q,callback]{select(q,callback);});
            });
        }
    }
    void frame() override {
        if(failed_&&connection_&&!busySQL()) {connection_->Destroy();connection_=nullptr;failed_=false;}
        refreshSQL(sql_);
        auto* h=SteamAPI_SteamGameServerHTTP();auto* u=SteamGameServerUtils();
        for(size_t i=0;i<http_.size();) {
            auto& r=http_[i];bool failed=false;
            const bool expired=Clock::now()-r.start>std::chrono::seconds(r.timeout+2);
            if(!expired&&h&&u&&!u->IsAPICallCompleted(r.call,&failed)) {++i;continue;}
            HTTPRequestCompleted_t result{};std::string body,error;
            if(expired||!h||!u||failed||!u->GetAPICallResult(r.call,&result,sizeof(result),HTTPRequestCompleted_t::k_iCallback,&failed)
               ||failed||!result.m_bRequestSuccessful) error="Global request failed or timed out.";
            else if(result.m_eStatusCode!=200) error="Global HTTP "+std::to_string(int(result.m_eStatusCode))+". The record or replay may be unavailable.";
            else if(result.m_unBodySize>r.limit) error="Global response exceeds the configured size limit.";
            else {
                body.resize(result.m_unBodySize);
                if(!h->GetHTTPResponseBodyData(r.handle,reinterpret_cast<uint8*>(body.data()),result.m_unBodySize)) error="Cannot read Global response.";
            }
            if(h) h->ReleaseHTTPRequest(r.handle);
            auto callback=std::move(r.callback);http_.erase(http_.begin()+i);
            ready_.push_back([callback,body=std::move(body),error=std::move(error)]() mutable {callback(std::move(body),std::move(error));});
        }
        auto done=std::move(ready_);ready_.clear();for(auto& fn:done) fn();
    }
    void shutdown() override {
        // Caller refuses unloading/reconfiguration while SQL callbacks still reference this code.
        if(connection_&&sql_) connection_->Destroy();
        connection_=nullptr;sql_=nullptr;connected_=false;
        if(auto* h=SteamAPI_SteamGameServerHTTP()) for(auto& r:http_) h->ReleaseHTTPRequest(r.handle);
        http_.clear();ready_.clear();
    }
};
} // namespace line
