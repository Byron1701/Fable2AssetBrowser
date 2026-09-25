#include "GDB/F3Gdb.h"
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

static void w16(std::vector<uint8_t>& b,uint16_t v){b.push_back(uint8_t(v));b.push_back(uint8_t(v>>8));}
static void w32(std::vector<uint8_t>& b,uint32_t v){for(int i=0;i<4;++i)b.push_back(uint8_t(v>>(i*8)));}

int main(){
    const uint32_t record_hash=0x12345678u;
    const std::string name="TEST";
    const uint32_t name_hash=F3Gdb::File::hash_string(name);
    std::vector<uint8_t> b(0x18,0);
    w32(b,0); w32(b,1); w32(b,8); w32(b,12); w32(b,1); w32(b,0);
    w32(b,0); w32(b,name_hash);
    b.push_back(0); b.push_back(1); w16(b,0);
    w32(b,0xAABBCCDD); w16(b,42); w16(b,0x0400);
    w32(b,record_hash); w16(b,7); w16(b,0);
    w32(b,record_hash); w32(b,name_hash);
    w32(b,0x00010000); w32(b,9); w32(b,1);
    w32(b,name_hash); b.insert(b.end(),name.begin(),name.end()); b.push_back(0);
    w32(b,0);
    F3Gdb::File file; std::string err;
    assert(file.Parse(b,err));
    assert(file.valid());
    assert(file.header().record_count==1);
    assert(file.row_types().size()==1);
    assert(file.records().size()==1);
    assert(file.records()[0].hash==record_hash);
    assert(file.records()[0].partition==7);
    assert(file.name_fnv_for_record(record_hash)==name_hash);
    const auto* resolved=file.name_for_record(record_hash);
    assert(resolved && resolved->text==name);
    return 0;
}
