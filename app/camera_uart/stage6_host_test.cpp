#include "photo_exif.h"
#include "trigger_frame_binder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

bool fail(const char *message)
{
    std::fprintf(stderr, "STAGE6_HOST_TEST_FAILED reason=\"%s\"\n", message);
    return false;
}

bool check_binder_lookup()
{
    trigger_frame_binder_config_t config = {};
    trigger_frame_binder_default_config(&config);
    trigger_frame_binder_t *binder = nullptr;
    if (trigger_frame_binder_create(&config, &binder) !=
        TRIGGER_FRAME_BINDER_OK) {
        return fail("create binder");
    }

    const trigger_frame_event_t frame0 = {0, 701, 0x2001, 1012000, 2012000};
    const bool ok =
        trigger_frame_binder_on_trigger(binder, 41, 1000000, 2000000, "SIM") ==
            TRIGGER_FRAME_BINDER_OK &&
        trigger_frame_binder_on_frame(binder, &frame0) ==
            TRIGGER_FRAME_BINDER_OK;
    if (!ok) {
        trigger_frame_binder_destroy(binder);
        return fail("bind camera 0 frame");
    }

    trigger_frame_match_t match0 = {};
    if (trigger_frame_binder_find_frame(binder, 0, 701, &match0) !=
            TRIGGER_FRAME_BINDER_OK ||
        !match0.valid || match0.pair_complete || match0.trigger_id != 41 ||
        match0.trigger_to_frame_ns != 12000 ||
        match0.realtime_dequeue_ns != 2012000 ||
        std::strcmp(match0.source, "SIM") != 0) {
        trigger_frame_binder_destroy(binder);
        return fail("lookup pending camera 0 frame");
    }

    const trigger_frame_event_t frame1 = {1, 801, 0x2001, 1015000, 2015000};
    if (trigger_frame_binder_on_frame(binder, &frame1) !=
        TRIGGER_FRAME_BINDER_OK) {
        trigger_frame_binder_destroy(binder);
        return fail("bind camera 1 frame");
    }
    trigger_frame_match_t match1 = {};
    const bool matches =
        trigger_frame_binder_find_frame(binder, 0, 701, &match0) ==
            TRIGGER_FRAME_BINDER_OK &&
        trigger_frame_binder_find_frame(binder, 1, 801, &match1) ==
            TRIGGER_FRAME_BINDER_OK &&
        match0.valid && match0.pair_complete && match1.valid &&
        match1.pair_complete && match1.trigger_id == 41 &&
        match1.trigger_to_frame_ns == 15000 &&
        match1.realtime_dequeue_ns == 2015000;
    trigger_frame_binder_destroy(binder);
    if (!matches)
        return fail("lookup completed dual-camera pair");
    return true;
}

}  // namespace

int main()
{
    std::string report;
    if (camera_photo::self_test(&report) != camera_photo::EXIF_OK) {
        fail(report.c_str());
        return EXIT_FAILURE;
    }
    if (!check_binder_lookup())
        return EXIT_FAILURE;
    std::printf("STAGE6_HOST_TEST_OK exif=\"%s\" "
                "binder_lookup=pending_and_completed_verified\n",
                report.c_str());
    return EXIT_SUCCESS;
}
