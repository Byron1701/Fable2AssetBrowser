#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
namespace F3Gdb {
enum class DataType : uint16_t { Bool=0x0000,Dword=0x0100,GroupIndex=0x0200,Float=0x0300,StringHash=0x0400,Enum=0x0500,ObjectHash=0x0600,ObjectLink=0x0700 };
struct Header { uint32_t tag=0,record_count=0,record_data_size=0,row_type_size=0,unique_record_count=0,padding=0; };
struct Section { size_t start=0,size=0; size_t end() const{return start+size;} };
struct Field { uint32_t column_hash=0,index=0; uint16_t data_id=0,data_type=0; };
struct RowType { uint32_t offset=0; uint8_t components=0,columns=0; uint16_t count2=0; std::vector<Field> fields; uint32_t total_columns() const{return uint32_t(columns)+uint32_t(count2)*256u;} size_t serialized_size() const{return 4u+fields.size()*8u;} };
struct Record { uint32_t index=0,row_type_offset=0,hash=0; uint16_t partition=0; const RowType* row_type=nullptr; std::vector<uint32_t> values; const Field* field(size_t i) const{return row_type&&i<row_type->fields.size()?&row_type->fields[i]:nullptr;} };
struct StringEntry { uint32_t hash=0,offset=0; std::string text; };
class File {
public:
 bool Parse(const std::vector<uint8_t>& bytes,std::string& error);
 bool valid() const{return valid_;}
 const Header& header() const{return header_;}
 const Section& record_data_section() const{return record_data_;}
 const Section& row_type_section() const{return row_types_;}
 const Section& record_hash_section() const{return record_hashes_;}
 const Section& partition_section() const{return partitions_;}
 const Section& record_fnv_section() const{return record_fnvs_;}
 const Section& string_data_section() const{return string_data_;}
 const Section& string_offset_section() const{return string_offsets_;}
 const std::vector<RowType>& row_types() const{return row_type_list_;}
 const std::vector<Record>& records() const{return records_;}
 const std::vector<StringEntry>& strings() const{return strings_;}
 const Record* record_by_hash(uint32_t hash) const;
 const StringEntry* string_by_hash(uint32_t hash) const;
 uint32_t name_fnv_for_record(uint32_t record_hash) const;
 const StringEntry* name_for_record(uint32_t record_hash) const;
 static uint32_t hash_string(const std::string& text);
 std::string validation_summary() const;
private:
 void clear();
 Header header_{}; Section record_data_{},row_types_{},record_hashes_{},partitions_{},record_fnvs_{},string_data_{},string_offsets_{};
 bool valid_=false;
 std::vector<RowType> row_type_list_; std::vector<Record> records_; std::vector<StringEntry> strings_;
 std::unordered_map<uint32_t,size_t> row_type_by_offset_,record_by_hash_,string_by_hash_;
 std::unordered_map<uint32_t,uint32_t> record_to_fnv_;
};
}