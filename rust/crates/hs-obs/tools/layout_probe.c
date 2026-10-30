/* Prints the libobs ABI facts rust/crates/hs-obs/src/sys.rs hard-codes (struct sizes, field offsets, enum values),
 * straight from the OBS headers, so a libobs bump is a diff of this output against sys.rs.
 *
 *   cl /nologo /TC /I external\obs-studio\libobs /I <dir with obsconfig.h> layout_probe.c
 *
 * obsconfig.h only needs the defines the C++ CMakeLists writes (OBS_DATA_PATH ...). */
#include <obs-module.h>
#include <stdio.h>
#include <stddef.h>

#define OFF(t, f) printf("offset " #t "." #f " = %zu\n", offsetof(struct t, f))
#define VAL(v) printf("value " #v " = %lld\n", (long long)(v))

int main(void)
{
	printf("libobs api %u.%u.%u = 0x%08x\n", LIBOBS_API_MAJOR_VER, LIBOBS_API_MINOR_VER, LIBOBS_API_PATCH_VER,
	       (unsigned)LIBOBS_API_VER);
	printf("size obs_source_info = %zu\n", sizeof(struct obs_source_info));
	OFF(obs_source_info, id);
	OFF(obs_source_info, type);
	OFF(obs_source_info, output_flags);
	OFF(obs_source_info, get_name);
	OFF(obs_source_info, create);
	OFF(obs_source_info, destroy);
	OFF(obs_source_info, get_defaults);
	OFF(obs_source_info, get_properties);
	OFF(obs_source_info, update);
	OFF(obs_source_info, audio_render);
	OFF(obs_source_info, enum_all_sources);
	OFF(obs_source_info, icon_type);
	OFF(obs_source_info, version);
	OFF(obs_source_info, get_dark_icon);
	OFF(obs_source_info, get_light_icon);
	printf("size obs_source_audio = %zu\n", sizeof(struct obs_source_audio));
	OFF(obs_source_audio, data);
	OFF(obs_source_audio, frames);
	OFF(obs_source_audio, speakers);
	OFF(obs_source_audio, format);
	OFF(obs_source_audio, samples_per_sec);
	OFF(obs_source_audio, timestamp);
	VAL(MAX_AV_PLANES);
	VAL(OBS_SOURCE_TYPE_INPUT);
	VAL(OBS_SOURCE_AUDIO);
	VAL(OBS_ICON_TYPE_AUDIO_INPUT);
	VAL(SPEAKERS_STEREO);
	VAL(AUDIO_FORMAT_FLOAT_PLANAR);
	VAL(OBS_MONITORING_TYPE_NONE);
	VAL(OBS_TEXT_DEFAULT);
	VAL(OBS_TEXT_INFO);
	VAL(OBS_TEXT_INFO_WARNING);
	VAL(OBS_COMBO_TYPE_LIST);
	VAL(OBS_COMBO_FORMAT_INT);
	VAL(LOG_WARNING);
	VAL(LOG_INFO);
	return 0;
}
