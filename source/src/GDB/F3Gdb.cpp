#include "F3Gdb.h"
#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <sstream>
namespace F3Gdb {
namespace {
uint16_t u16(const uint8_t*p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
uint32_t u32(const uint8_t*p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
bool range(size_t o,size_t n,size_t z){return o<=z&&n<=z-o;}
bool known(uint16_t t){return t==0||t==0x100||t==0x200||t==0x300||t==0x400||t==0x500||t==0x600||t==0x700;}
}
void File::clear(){header_={};record_data_={};row_types_={};record_hashes_={};partitions_={};record_fnvs_={};string_data_={};string_offsets_={};valid_=false;row_type_list_.clear();records_.clear();strings_.clear();row_type_by_offset_.clear();record_by_hash_.clear();string_by_hash_.clear();record_to_fnv_.clear();}
uint32_t File::hash_string(const std::string&s){
 uint32_t h=0x811C9DC5u; size_t i=0;
 auto mix=[&](uint32_t x){h*=0x01000193u;h^=x&0xffffu;};
 while(i<s.size()){uint32_t cp=0xfffdu;uint8_t c=(uint8_t)s[i++];
  if(c<0x80)cp=c;
  else if((c&0xe0)==0xc0&&i<s.size()){uint8_t a=(uint8_t)s[i];if((a&0xc0)==0x80){++i;cp=((c&0x1f)<<6)|(a&0x3f);if(cp<0x80)cp=0xfffd;}}
  else if((c&0xf0)==0xe0&&i+1<s.size()){uint8_t a=(uint8_t)s[i],b=(uint8_t)s[i+1];if((a&0xc0)==0x80&&(b&0xc0)==0x80){i+=2;cp=((c&15)<<12)|((a&63)<<6)|(b&63);if(cp<0x800)cp=0xfffd;}}
  else if((c&0xf8)==0xf0&&i+2<s.size()){uint8_t a=(uint8_t)s[i],b=(uint8_t)s[i+1],d=(uint8_t)s[i+2];if((a&0xc0)==0x80&&(b&0xc0)==0x80&&(d&0xc0)==0x80){i+=3;cp=((c&7)<<18)|((a&63)<<12)|((b&63)<<6)|(d&63);if(cp<0x10000||cp>0x10ffff)cp=0xfffd;}}
  if(cp<=0xffff)mix(cp);else{cp-=0x10000;mix(0xd800+(cp>>10));mix(0xdc00+(cp&0x3ff));}
 } return h;
}
bool File::Parse(const std::vector<uint8_t>&b,std::string&e){
 clear();e.clear();constexpr size_t H=0x18;
 if(b.size()<H){e="F3 GDB: file smaller than 0x18-byte header";return false;}
 header_.tag=u32(b.data());header_.record_count=u32(b.data()+4);header_.record_data_size=u32(b.data()+8);header_.row_type_size=u32(b.data()+12);header_.unique_record_count=u32(b.data()+16);header_.padding=u32(b.data()+20);
 if(header_.tag!=0){e="F3 GDB: header tag is not zero";return false;}
 size_t rd=H,rt=rd+header_.record_data_size,rh=rt+header_.row_type_size,pa=rh+size_t(header_.record_count)*4,pe=pa+size_t(header_.record_count)*2,map=pe+(header_.record_count&1?2:0),mapsz=size_t(header_.unique_record_count)*8,sh=map+mapsz;
 if(!range(rd,header_.record_data_size,b.size())||!range(rt,header_.row_type_size,b.size())||!range(rh,size_t(header_.record_count)*4,b.size())||!range(pa,size_t(header_.record_count)*2,b.size())||!range(map,mapsz,b.size())||!range(sh,12,b.size())){e="F3 GDB: section arithmetic exceeds file size";return false;}
 record_data_={rd,header_.record_data_size};row_types_={rt,header_.row_type_size};record_hashes_={rh,size_t(header_.record_count)*4};partitions_={pa,size_t(header_.record_count)*2};record_fnvs_={map,mapsz};
 for(size_t o=0;o<row_types_.size;){
  if(row_types_.size-o<4){e="F3 GDB: truncated row-type header";return false;}
  RowType r;r.offset=(uint32_t)o;r.components=b[rt+o];r.columns=b[rt+o+1];r.count2=u16(b.data()+rt+o+2);uint32_t n=r.total_columns();size_t sz=4+size_t(n)*8;if(sz>row_types_.size-o){e="F3 GDB: row-type exceeds section";return false;}
  r.fields.reserve(n);size_t hb=o+4,mb=hb+size_t(n)*4;
  for(uint32_t i=0;i<n;++i){Field f;f.index=i;f.column_hash=u32(b.data()+rt+hb+size_t(i)*4);f.data_id=u16(b.data()+rt+mb+size_t(i)*4);f.data_type=u16(b.data()+rt+mb+size_t(i)*4+2);if(!known(f.data_type)){std::ostringstream s;s<<"F3 GDB: unknown datatype 0x"<<std::hex<<std::uppercase<<f.data_type;e=s.str();return false;}r.fields.push_back(f);}
  row_type_by_offset_[r.offset]=row_type_list_.size();row_type_list_.push_back(std::move(r));o+=sz;
 }
 size_t cur=rd;records_.reserve(header_.record_count);
 for(uint32_t i=0;i<header_.record_count;++i){if(cur+4>record_data_.end()){e="F3 GDB: truncated record";return false;}uint32_t rel=u32(b.data()+cur);auto it=row_type_by_offset_.find(rel);if(it==row_type_by_offset_.end()){e="F3 GDB: record references unknown row type";return false;}const RowType&rtx=row_type_list_[it->second];size_t sz=4+rtx.fields.size()*4;if(sz>record_data_.end()-cur){e="F3 GDB: record exceeds record-data section";return false;}Record r;r.index=i;r.row_type_offset=rel;r.row_type=&rtx;r.values.reserve(rtx.fields.size());for(size_t f=0;f<rtx.fields.size();++f)r.values.push_back(u32(b.data()+cur+4+f*4));records_.push_back(std::move(r));cur+=sz;}
 if(cur!=record_data_.end()){e="F3 GDB: record-data does not tile section";return false;}
 for(uint32_t i=0;i<header_.record_count;++i){records_[i].hash=u32(b.data()+rh+size_t(i)*4);records_[i].partition=u16(b.data()+pa+size_t(i)*2);if(!record_by_hash_.emplace(records_[i].hash,i).second){e="F3 GDB: duplicate record hash";return false;}}
 for(uint32_t i=0;i<header_.unique_record_count;++i){size_t o=map+size_t(i)*8;record_to_fnv_[u32(b.data()+o)]=u32(b.data()+o+4);}
 uint32_t ver=u32(b.data()+sh),ds=u32(b.data()+sh+4),sc=u32(b.data()+sh+8);if(ver!=0x10000){e="F3 GDB: unsupported string-table version";return false;}
 size_t sd=sh+12,so=sd+size_t(ds);if(!range(sd,ds,b.size())||!range(so,size_t(sc)*4,b.size())){e="F3 GDB: string table exceeds file size";return false;}string_data_={sd,ds};string_offsets_={so,size_t(sc)*4};strings_.reserve(sc);
 std::unordered_map<uint32_t,bool> offs;offs.reserve(size_t(sc)*2);
 for(uint32_t i=0;i<sc;++i){uint32_t o=u32(b.data()+so+size_t(i)*4);if(o>=ds||!offs.emplace(o,true).second){e="F3 GDB: invalid or duplicate string offset";return false;}size_t a=sd+o;if(a+4>string_data_.end()){e="F3 GDB: truncated string entry";return false;}size_t z=a+4;while(z<string_data_.end()&&b[z])++z;if(z>=string_data_.end()){e="F3 GDB: unterminated string entry";return false;}StringEntry x;x.hash=u32(b.data()+a);x.offset=o;x.text.assign((const char*)b.data()+a+4,z-a-4);strings_.push_back(std::move(x));string_by_hash_.emplace(strings_.back().hash,strings_.size()-1);}
 valid_=true;return true;
}
const Record* File::record_by_hash(uint32_t h)const{auto i=record_by_hash_.find(h);return i==record_by_hash_.end()?nullptr:&records_[i->second];}
const StringEntry* File::string_by_hash(uint32_t h)const{auto i=string_by_hash_.find(h);return i==string_by_hash_.end()?nullptr:&strings_[i->second];}
uint32_t File::name_fnv_for_record(uint32_t h)const{auto i=record_to_fnv_.find(h);return i==record_to_fnv_.end()?0:i->second;}
const StringEntry* File::name_for_record(uint32_t h)const{uint32_t f=name_fnv_for_record(h);return f?string_by_hash(f):nullptr;}
std::string File::validation_summary()const{if(!valid_)return "F3 GDB: invalid/unparsed";size_t named=0;for(const auto&r:records_)if(name_for_record(r.hash))++named;std::ostringstream s;s<<"F3 GDB valid: "<<records_.size()<<" records, "<<row_type_list_.size()<<" row types, "<<strings_.size()<<" strings, "<<record_to_fnv_.size()<<" record->FNV mappings, "<<named<<" locally named records; record-data="<<record_data_.size<<", row-types="<<row_types_.size<<", file-end="<<string_offsets_.end();return s.str();}
}