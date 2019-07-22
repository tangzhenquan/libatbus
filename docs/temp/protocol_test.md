
flatbuffers
------------

```cpp
std::vector<unsigned char> packed_buffer;
char test_buffer[] = "hello world!";

{
    ::flatbuffers::FlatBufferBuilder fbb;

    uint64_t self_id = 0x12345678;
    uint32_t flags   = 0;
    flags |= atbus::protocol::ATBUS_FORWARD_DATA_FLAG_TYPE_REQUIRE_RSP;

    fbb.Finish(::atbus::protocol::Createmsg(fbb,
                                    ::atbus::protocol::Createmsg_head(fbb, ::atbus::protocol::ATBUS_PROTOCOL_CONST_ATBUS_PROTOCOL_VERSION,
                                                                    123, 0, 9876543210, self_id),
                                    ::atbus::protocol::msg_body_data_transform_req,
                                    ::atbus::protocol::Createforward_data(fbb, 0x123456789, 0x987654321, fbb.CreateVector(&self_id, 1),
                                                                        fbb.CreateVector(reinterpret_cast<const uint8_t *>(test_buffer), sizeof(test_buffer)),
                                                                        flags)
                                        .Union())

    );
    packed_buffer.assign(reinterpret_cast<const unsigned char *>(fbb.GetBufferPointer()), 
        reinterpret_cast<const unsigned char *>(fbb.GetBufferPointer()) + fbb.GetSize());
    std::stringstream so;
    util::string::serialization(packed_buffer.data(), packed_buffer.size(), so);
    CASE_MSG_INFO() << "flatbuffers encoded(size=" << packed_buffer.size() << "): " << so.str() << std::endl;
}
```

[ RUNNING  ] flatbuffers encoded(size=160): \020\000\000\000\000\000\012\000\022\000\010\000\007\000\014\000\012\000\000\000\000\000\000\003l\000\000\000\024\000\000\000\000\000\016\000 \000\020\000\030\000\004\000\010\000\014\000\016\000\000\0000\000\000\000\030\000\000\000\002\000\000\000\611gE#\001\000\000\000!Ce\607\011\000\000\000\015\000\000\000hello world!\000\000\000\000\001\000\000\000xV4\022\000\000\000\000\000\000\000\000\000\000\016\000\034\000\004\000\010\000\000\000\014\000\024\000\016\000\000\000\002\000\000\000{\000\000\000\752\026\660L\002\000\000\000xV4\022\000\000\000\000

