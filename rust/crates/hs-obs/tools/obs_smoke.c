/* Loads the built module into a real libobs core (no UI, no video) and exercises it the way OBS does:
 * register the source, create it with fresh settings, read the saved defaults and the properties, let the feed
 * thread run, destroy it. Needs an OBS Studio install (obs.dll and its folder) and the libobs headers.
 *
 *   dumpbin /exports "C:\Program Files\obs-studio\bin\64bit\obs.dll"   -> obs.def (LIBRARY obs / EXPORTS ...)
 *   lib /nologo /def:obs.def /machine:x64 /out:obs.lib
 *   cl /nologo /TC /I external\obs-studio\libobs /I <dir with obsconfig.h> obs_smoke.c obs.lib /Fe:obs_smoke.exe
 *   set PATH=C:\Program Files\obs-studio\bin\64bit;%PATH%
 *   obs_smoke.exe "C:\Program Files\obs-studio\bin\64bit" rust\target\release\hs_obs.dll
 *
 * With `--hub` as a third argument it also expects audio: start `cargo run --release --example tone_hub -p hs-obs`
 * (a stand-in Hub on bus Main, 10 s) just before, and the source must deliver its tone to OBS.
 *
 * (the C++ CMake does the same def -> lib step, see obs/hearaside-obs/CMakeLists.txt). Prints the module's log
 * lines and one `ok` / `FAIL` line per check; the exit code is the number of failures. */
#include <obs.h>
#include <obs-module.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, what)                                              \
	do {                                                        \
		if (c) printf("ok   %s\n", what);                   \
		else { printf("FAIL %s\n", what); ++fails; }        \
	} while (0)

static float peak = 0.0f;
static int blocks = 0;

static void capture(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	(void)param; (void)source; (void)muted;
	const float *l = (const float *)audio->data[0];
	if (!l) return;
	++blocks;
	for (uint32_t i = 0; i < audio->frames; ++i)
		if (l[i] > peak) peak = l[i];
}

static void log_handler(int level, const char *msg, va_list args, void *param)
{
	char buf[1024];
	(void)param;
	vsnprintf(buf, sizeof buf, msg, args);
	if (strstr(buf, "HEARASIDE")) printf("log  [%d] %s\n", level, buf);
}

int main(int argc, char **argv)
{
	if (argc < 3) return 2;
	wchar_t dir[MAX_PATH];
	MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, dir, MAX_PATH);
	SetDllDirectoryW(dir);
	SetCurrentDirectoryW(dir);

	base_set_log_handler(log_handler, NULL);
	CHECK(obs_startup("en-US", NULL, NULL), "obs_startup");
	struct obs_audio_info ai = {.samples_per_sec = 44100, .speakers = SPEAKERS_STEREO};
	CHECK(obs_reset_audio(&ai), "obs_reset_audio (44.1 kHz)");

	obs_module_t *module = NULL;
	CHECK(obs_open_module(&module, argv[2], NULL) == MODULE_SUCCESS, "obs_open_module");
	obs_init_module(module);
	obs_post_load_modules();
	CHECK(obs_get_module_author(module) && !strcmp(obs_get_module_author(module), "HEARASIDE"), "module author");
	CHECK(obs_get_module_name(module) && !strcmp(obs_get_module_name(module), "HEARASIDE"), "module name");
	printf("info libobs 0x%08x\n", obs_get_version());

	const char *id = NULL;
	bool registered = false;
	for (size_t i = 0; obs_enum_input_types(i, &id); ++i)
		if (!strcmp(id, "hearaside_source")) registered = true;
	CHECK(registered, "source id hearaside_source registered");
	CHECK(!strcmp(obs_source_get_display_name("hearaside_source"), "HEARASIDE"), "display name");
	CHECK((obs_get_source_output_flags("hearaside_source") & OBS_SOURCE_AUDIO) != 0, "OBS_SOURCE_AUDIO flag");

	obs_source_t *src = obs_source_create("hearaside_source", "smoke", NULL, NULL);
	CHECK(src != NULL, "obs_source_create");
	obs_data_t *settings = obs_source_get_settings(src);
	CHECK(!strcmp(obs_data_get_string(settings, "bus"), "Main"), "default bus = Main");
	CHECK(obs_data_get_int(settings, "output") == 0, "default output = 0");
	CHECK(obs_data_get_int(settings, "buffer_ms") == 0, "default buffer_ms = 0");
	CHECK(obs_data_get_bool(settings, "monitoring_checked"), "monitoring_checked saved");
	CHECK(obs_source_get_monitoring_type(src) == OBS_MONITORING_TYPE_NONE, "audio monitoring off");
	obs_data_release(settings);

	obs_properties_t *props = obs_source_properties(src);
	CHECK(props != NULL, "properties");
	const char *names[] = {"bus", "output", "buffer_ms", "status_info", "refresh"};
	for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
		char what[64];
		snprintf(what, sizeof what, "property %s", names[i]);
		CHECK(obs_properties_get(props, names[i]) != NULL, what);
	}
	obs_property_t *buf = obs_properties_get(props, "buffer_ms");
	CHECK(buf && obs_property_list_item_count(buf) == 4 && obs_property_list_item_int(buf, 1) == 15 &&
		      obs_property_list_item_int(buf, 3) == 60,
	      "buffer list 0/15/30/60");
	obs_property_t *out = obs_properties_get(props, "output");
	CHECK(out && obs_property_list_item_count(out) == 9, "output list: Stream Mix + 8 stems");
	obs_property_t *st = obs_properties_get(props, "status_info");
	if (st) printf("info status: %s\n", obs_property_description(st));
	obs_properties_destroy(props);

	int expect_hub = argc > 3 && !strcmp(argv[3], "--hub");
	obs_set_output_source(0, src);   /* makes the source active, so OBS's audio thread mixes it */
	obs_source_add_audio_capture_callback(src, capture, NULL);
	Sleep(expect_hub ? 4000 : 600);   /* feed thread: ~120 ticks per 600 ms */
	if (expect_hub) {
		obs_properties_t *p2 = obs_source_properties(src);
		printf("info status: %s\n", obs_property_description(obs_properties_get(p2, "status_info")));
		obs_properties_destroy(p2);
		printf("info audio blocks %d, peak %.3f\n", blocks, peak);
		CHECK(peak > 0.05f && peak < 0.3f, "the Hub's 440 Hz tone (0.2) reaches OBS");
	} else {
		CHECK(peak == 0.0f, "no Hub: silence");
	}
	obs_source_remove_audio_capture_callback(src, capture, NULL);
	obs_set_output_source(0, NULL);
	obs_source_release(src);
	CHECK(true, "source destroyed (feed thread joined)");

	obs_shutdown();
	printf("%s\n", fails ? "SMOKE FAILED" : "SMOKE OK");
	return fails;
}
