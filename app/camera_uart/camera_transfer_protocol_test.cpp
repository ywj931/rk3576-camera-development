#include "camera_transfer_protocol.h"

#include <array>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

bool digest_matches(const std::array<std::uint8_t, 32> &digest,
                    const char *expected)
{
    return camera_transfer::sha256_hex(digest) == expected;
}

}  // namespace

int main()
{
    char path[] = "/tmp/camera_transfer_hash.XXXXXX";
    const int file = mkstemp(path);
    if (file < 0 || write(file, "abc", 3) != 3 || close(file) != 0)
        return 1;
    std::array<std::uint8_t, 32> digest{};
    std::uint64_t size = 0;
    std::string error;
    const int hash_result =
        camera_transfer::sha256_file(path, &digest, &size, &error);
    unlink(path);
    if (hash_result != camera_transfer::ok || size != 3U ||
        !digest_matches(
            digest,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"))
        return 1;

    camera_transfer::photo_header header;
    header.format = camera_transfer::photo_format::jpeg;
    header.camera_id = 0;
    header.session_id = 0x1122334455667788ULL;
    header.frame_id = 123;
    header.exposure_start_utc_ns = 1720000000123456789ULL;
    header.exposure_duration_ns = 5000000ULL;
    header.payload_size = size;
    header.sha256 = digest;
    std::array<std::uint8_t, camera_transfer::kPhotoHeaderSize> wire{};
    camera_transfer::photo_header decoded;
    if (camera_transfer::encode_photo_header(header, &wire) !=
            camera_transfer::ok ||
        camera_transfer::decode_photo_header(wire.data(), wire.size(),
                                             &decoded) != camera_transfer::ok ||
        decoded.format != header.format ||
        decoded.camera_id != header.camera_id ||
        decoded.session_id != header.session_id ||
        decoded.frame_id != header.frame_id ||
        decoded.exposure_start_utc_ns != header.exposure_start_utc_ns ||
        decoded.exposure_duration_ns != header.exposure_duration_ns ||
        decoded.payload_size != header.payload_size ||
        decoded.sha256 != header.sha256)
        return 1;

    const auto valid_wire = wire;
    wire[4]++;
    if (camera_transfer::decode_photo_header(wire.data(), wire.size(),
                                             &decoded) !=
        camera_transfer::err_protocol)
        return 1;
    wire = valid_wire;
    wire[40] = 0xff;
    if (camera_transfer::decode_photo_header(wire.data(), wire.size(),
                                             &decoded) !=
        camera_transfer::err_protocol)
        return 1;

    wire = valid_wire;
    wire[6] = 0xfe;
    if (camera_transfer::decode_photo_header(wire.data(), wire.size(),
                                             &decoded) != camera_transfer::ok ||
        camera_transfer::supported_format(decoded.format))
        return 1;

    camera_transfer::ack response;
    response.status = camera_transfer::ack_status::sha256_mismatch;
    response.camera_id = 0;
    response.session_id = header.session_id;
    response.frame_id = header.frame_id;
    std::array<std::uint8_t, camera_transfer::kAckSize> ack_wire{};
    camera_transfer::ack decoded_ack;
    if (camera_transfer::encode_ack(response, &ack_wire) !=
            camera_transfer::ok ||
        camera_transfer::decode_ack(ack_wire.data(), ack_wire.size(),
                                    &decoded_ack) != camera_transfer::ok ||
        decoded_ack.status != response.status ||
        decoded_ack.camera_id != response.camera_id ||
        decoded_ack.session_id != response.session_id ||
        decoded_ack.frame_id != response.frame_id)
        return 1;

    std::printf(
        "CAMERA_TRANSFER_PROTOCOL_TEST_OK header=%zu ack=%zu "
        "identity=session,camera,frame integrity=length,sha256 "
        "unknown_format=nackable\n",
        camera_transfer::kPhotoHeaderSize, camera_transfer::kAckSize);
    return 0;
}
