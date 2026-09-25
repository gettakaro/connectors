// One-shot PCRE2 half of the legacy-JavaScript grammar comparison. It is an
// operator migration tool, never linked into or run by the game process.
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <iterator>
#include <string>

using Json=nlohmann::json;
int main(){
    const std::string input((std::istreambuf_iterator<char>(std::cin)),std::istreambuf_iterator<char>());
    Json request=Json::parse(input,nullptr,false),results=Json::object();
    if(!request.is_object()||!request.contains("patterns")||!request["patterns"].is_object()||
       !request.contains("fixtures")||!request["fixtures"].is_object())return 2;
    for(auto it=request["patterns"].begin();it!=request["patterns"].end();++it){
        const std::string key=it.key(),pattern=it.value().get<std::string>();
        Json rows=Json::array();int error=0;PCRE2_SIZE errorOffset=0;
        pcre2_code* code=pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),pattern.size(),
                                       PCRE2_CASELESS|PCRE2_UTF,&error,&errorOffset,nullptr);
        if(!code){PCRE2_UCHAR message[256];pcre2_get_error_message(error,message,sizeof message);
            results[key]={{"error",std::string(reinterpret_cast<char*>(message))+" at "+std::to_string(errorOffset)}};continue;}
        auto* context=pcre2_match_context_create(nullptr);
        pcre2_set_match_limit(context,200000);pcre2_set_depth_limit(context,1000);
        uint32_t captureCount=0,nameCount=0,nameEntrySize=0;PCRE2_SPTR nameTable=nullptr;
        pcre2_pattern_info(code,PCRE2_INFO_CAPTURECOUNT,&captureCount);
        pcre2_pattern_info(code,PCRE2_INFO_NAMECOUNT,&nameCount);
        if(nameCount){pcre2_pattern_info(code,PCRE2_INFO_NAMETABLE,&nameTable);
                      pcre2_pattern_info(code,PCRE2_INFO_NAMEENTRYSIZE,&nameEntrySize);}
        for(const auto& fixture:request["fixtures"].value(key,Json::array())){
            const std::string line=fixture.get<std::string>();
            auto* match=pcre2_match_data_create_from_pattern(code,nullptr);
            const int rc=pcre2_match(code,reinterpret_cast<PCRE2_SPTR>(line.data()),line.size(),0,0,match,context);
            if(rc==PCRE2_ERROR_MATCHLIMIT||rc==PCRE2_ERROR_DEPTHLIMIT){rows.push_back({{"error","PCRE2 match/depth limit"}});pcre2_match_data_free(match);continue;}
            if(rc<0){rows.push_back({{"matched",false},{"captures",Json::array()},{"named",Json::object()}});pcre2_match_data_free(match);continue;}
            PCRE2_SIZE* offsets=pcre2_get_ovector_pointer(match);
            Json captures=Json::array(),named=Json::object();
            for(uint32_t n=1;n<=captureCount;n++)captures.push_back(offsets[2*n]==PCRE2_UNSET?Json(nullptr):Json(line.substr(offsets[2*n],offsets[2*n+1]-offsets[2*n])));
            for(uint32_t i=0;i<nameCount;i++){
                PCRE2_SPTR entry=nameTable+i*nameEntrySize;
                const uint32_t n=(entry[0]<<8)|entry[1];
                const std::string name(reinterpret_cast<const char*>(entry+2));
                named[name]=offsets[2*n]==PCRE2_UNSET?Json(nullptr):Json(line.substr(offsets[2*n],offsets[2*n+1]-offsets[2*n]));
            }
            rows.push_back({{"matched",true},{"full",line.substr(offsets[0],offsets[1]-offsets[0])},
                            {"captures",captures},{"named",named}});
            pcre2_match_data_free(match);
        }
        results[key]={{"rows",rows}};
        pcre2_match_context_free(context);pcre2_code_free(code);
    }
    std::cout<<results.dump()<<'\n';return 0;
}
