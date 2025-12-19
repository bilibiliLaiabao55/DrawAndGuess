#include <tremor/ivorbisfile.h>
#include <tremor/ivorbiscodec.h>
#include <citro2d.h>
#include <citro3d.h>
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <libmodplug/modplug.h>
#include <malloc.h>
#include <string.h>
// ---- DEFINITIONS ----
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
// ---- DEFINITIONS ----
#define ogg_chn 0
Thread threadId;
bool vorbis_playing, mod_playing;
OggVorbis_File vorbisFile;

static const int THREAD_AFFINITY = -1;        // Execute thread on any core
static const int THREAD_STACK_SZ = 32 * 1024; // 32kB stack for audio thread

// ---- END DEFINITIONS ----

ndspWaveBuf s_waveBufs[3];
int16_t *s_audioBuffer = NULL;

LightEvent s_event;
volatile bool s_quit = false; // Quit flag

// Copied from audio example with a slight bit of change

// Pause until user presses a button

// ---- END HELPER FUNCTIONS ----

// Audio initialisation code
// This sets up NDSP and our primary audio buffer
void audioCallback_mod(void *const data);
bool audioInit(OggVorbis_File *vorbisFile_)
{
    vorbis_info *vi = ov_info(vorbisFile_, -1);

    // Setup NDSP
    ndspChnReset(ogg_chn);
    ndspChnSetInterp(ogg_chn, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(ogg_chn, vi->rate);
    ndspChnSetFormat(ogg_chn, vi->channels == 1
                                  ? NDSP_FORMAT_MONO_PCM16
                                  : NDSP_FORMAT_STEREO_PCM16);

    // Allocate audio buffer
    // 120ms buffer
    const size_t SAMPLES_PER_BUF = vi->rate * 120 / 1000;
    // mono (1) or stereo (2)
    const size_t CHANNELS_PER_SAMPLE = vi->channels;
    // s16 buffer
    const size_t WAVEBUF_SIZE = SAMPLES_PER_BUF * CHANNELS_PER_SAMPLE * sizeof(s16);
    const size_t bufferSize = WAVEBUF_SIZE * ARRAY_SIZE(s_waveBufs);
    s_audioBuffer = (int16_t *)linearAlloc(bufferSize);
    // Setup waveBufs for NDSP
    memset(&s_waveBufs, 0, sizeof(s_waveBufs));
    int16_t *buffer = s_audioBuffer;

    for (size_t i = 0; i < ARRAY_SIZE(s_waveBufs); ++i)
    {
        s_waveBufs[i].data_vaddr = buffer;
        s_waveBufs[i].nsamples = WAVEBUF_SIZE / sizeof(buffer[0]);
        s_waveBufs[i].status = NDSP_WBUF_DONE;

        buffer += WAVEBUF_SIZE / sizeof(buffer[0]);
    }

    return true;
}

// Audio de-initialisation code
// Stops playback and frees the primary audio buffer
void audioExit(void)
{
    ndspChnReset(ogg_chn);
    linearFree(s_audioBuffer);
}

// Main audio decoding logic
// This function pulls and decodes audio samples from vorbisFile_ to fill waveBuf_
bool fillBuffer(OggVorbis_File *vorbisFile_, ndspWaveBuf *waveBuf_)
{

    // Decode (2-byte) samples until our waveBuf is full
    int totalBytes = 0;
    while (totalBytes < waveBuf_->nsamples * sizeof(s16))
    {
        int16_t *buffer = waveBuf_->data_pcm16 + (totalBytes / sizeof(s16));
        const size_t bufferSize = (waveBuf_->nsamples * sizeof(s16) - totalBytes);

        // Decode bufferSize bytes from vorbisFile_ into buffer,
        // storing the number of bytes that were read (or error)
        const int bytesRead = ov_read(vorbisFile_, (char *)buffer, bufferSize, NULL);
        if (bytesRead <= 0)
        {
            if (bytesRead == 0)
                break; // No error here
            break;
        }

        totalBytes += bytesRead;
    }

    // Pass samples to NDSP
    // this calculation will make a number <= the previous nsamples
    // = for most cases
    // < for the last possible chunk of the file, which may have less samples before EOF
    // after which we don't care to recover the length
    waveBuf_->nsamples = totalBytes / sizeof(s16);
    ndspChnWaveBufAdd(ogg_chn, waveBuf_);
    DSP_FlushDataCache(waveBuf_->data_pcm16, totalBytes);
    return true;
}

// NDSP audio frame callback
// This signals the audioThread to decode more things
// once NDSP has played a sound frame, meaning that there should be
// one or more available waveBufs to fill with more data.
void audioCallback_ogg(void *const nul_)
{
    (void)nul_; // Unused

    if (s_quit)
    { // Quit flag
        return;
    }

    LightEvent_Signal(&s_event);
}

// Audio thread
// This handles calling the decoder function to fill NDSP buffers as necessary
void audioThread(void *const vorbisFile_)
{
    OggVorbis_File *const vorbisFile = (OggVorbis_File *)vorbisFile_;

    while (!s_quit)
    { // Whilst the quit flag is unset,
      // search our waveBufs and fill any that aren't currently
      // queued for playback (i.e, those that are 'done')
        if (mod_playing)
        {
            audioCallback_mod(NULL);
            audioCallback_mod(NULL);
        }
        if (vorbis_playing)
        {
            for (size_t i = 0; i < ARRAY_SIZE(s_waveBufs); ++i)
            {
                if (s_waveBufs[i].status != NDSP_WBUF_DONE)
                {
                    continue;
                }
                if (!fillBuffer(vorbisFile, &s_waveBufs[i]))
                { // Playback complete
                    vorbis_playing = false;
                }
            }
        }
        // Wait for a signal that we're needed again before continuing,
        // so that we can yield to other things that want to run
        // (Note that the 3DS uses cooperative threading)
        LightEvent_Wait(&s_event);
    }
}

void play_vorbis(char *restrict song_path)
{
    vorbis_playing = true;
    // Open the Ogg Vorbis audio file
    FILE *fh = fopen(song_path, "rb");
    ov_open(fh, &vorbisFile, NULL, 0);

    // Attempt audioInit
    audioInit(&vorbisFile);

    // Set the ndsp sound frame callback which signals our audioThread
    ndspSetCallback(audioCallback_ogg, NULL);
}
typedef struct
{
    ModPlugFile *plug;
    ModPlug_Settings settings;
} ModplugDecoder;
#define mod_chn 1
// roughly a video frame's worth of audio
static const size_t decoderBufSize = 800 * 2 * 2;
static ModplugDecoder decoder;
static ndspWaveBuf wavebufs[2];
static int nextBuf = 0;

void audioCallback_mod(void *const data)
{
    if (wavebufs[nextBuf].status == NDSP_WBUF_DONE)
    {
        size_t decoded = ModPlug_Read(decoder.plug, wavebufs[nextBuf].data_pcm16, decoderBufSize);
        if (decoded != 0)
        {
            wavebufs[nextBuf].nsamples = ((decoded / 2) / sizeof(int16_t));
            DSP_FlushDataCache(wavebufs[nextBuf].data_pcm16, decoded);
            ndspChnWaveBufAdd(mod_chn, &wavebufs[nextBuf]);
            nextBuf ^= 1;
        }
    }
}

void play_mod(const char *__restrict path)
{
    mod_playing = true;
    decoder.settings.mFlags = MODPLUG_ENABLE_OVERSAMPLING | MODPLUG_ENABLE_NOISE_REDUCTION;
    decoder.settings.mChannels = 2;
    decoder.settings.mBits = 16;
    decoder.settings.mFrequency = 44100;
    decoder.settings.mResamplingMode = MODPLUG_RESAMPLE_LINEAR;

    /* Fill with modplug defaults */
    decoder.settings.mStereoSeparation = 128;
    decoder.settings.mMaxMixChannels = 32;
    decoder.settings.mReverbDepth = 0;
    decoder.settings.mReverbDelay = 0;
    decoder.settings.mBassAmount = 0;
    decoder.settings.mBassRange = 0;
    decoder.settings.mSurroundDepth = 0;
    decoder.settings.mSurroundDelay = 0;
    decoder.settings.mLoopCount = -1;

    ModPlug_SetSettings(&decoder.settings);

    struct stat fileStat;
    stat(path, &fileStat);
    size_t bufferSize = fileStat.st_size;

    FILE *file = fopen(path, "rb");

    void *buffer = (void *)malloc(bufferSize);
    fread(buffer, bufferSize, 1, file);

    decoder.plug = ModPlug_Load(buffer, bufferSize);

    free(buffer);
    fclose(file);

    if (decoder.plug == 0)
    {
        printf("Couldn't load mod file!\n");
    }
    else
    {
        ModPlug_SetMasterVolume(decoder.plug, 128);

        ndspChnReset(mod_chn);
        ndspChnSetFormat(mod_chn, NDSP_FORMAT_STEREO_PCM16);
        ndspChnSetRate(mod_chn, 44100);
        ndspChnSetInterp(mod_chn, NDSP_INTERP_POLYPHASE);

        // Set up audiobuffers using linearAlloc
        // This ensures audio data is in contiguous physical ram
        wavebufs[0].data_pcm16 = (int16_t *)linearAlloc(decoderBufSize);
        wavebufs[0].looping = false;
        wavebufs[0].status = NDSP_WBUF_DONE;

        wavebufs[1].data_pcm16 = (int16_t *)linearAlloc(decoderBufSize);
        wavebufs[1].looping = false;
        wavebufs[1].status = NDSP_WBUF_DONE;

        // Fill the two audio buffers
        // audioCallback_mod(NULL);
        // audioCallback_mod(NULL);

        // // and chain the rest of the audio using the callback
        // ndspSetCallback(audioCallback_mod, NULL);
        ndspSetCallback(audioCallback_ogg, NULL);
    }
}
void create_audio_thread(void)
{
    LightEvent_Init(&s_event, RESET_ONESHOT);
    // Spawn audio thread

    // Set the thread priority to the main thread's priority ...
    int32_t priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // ... then subtract 1, as lower number => higher actual priority ...
    priority -= 1;
    // ... finally, clamp it between 0x18 and 0x3F to guarantee that it's valid.
    priority = priority < 0x18 ? 0x18 : priority;
    priority = priority > 0x3F ? 0x3F : priority;

    // Start the thread, passing the address of our vorbisFile as an argument.
    threadId = threadCreate(audioThread, &vorbisFile,
                            THREAD_STACK_SZ, priority,
                            THREAD_AFFINITY, false);
}
u8 style;
C2D_Image style_bg;
C2D_Image title_image;
C2D_Image title_options;
u8 end;
s8 style_anim_timer0;
bool disable_title_bump;
u8 temp0;
float temp1;
u8 state;
float title_bump;
bool title_bump_up;
u32 keyDown;
u32 keyHeld;
static char text[60];
C3D_RenderTarget *top_render_target;
C3D_RenderTarget *bottom_render_target;
s64 timer0, timer1, timer2;
s64 timer0_target, timer1_target, timer2_target;
u8 language;
touchPosition touch;
bool enable_nfc;
bool check_touched(u16 x, u16 y, u16 width, u16 height)
{
    if (touch.px > x && touch.px < x + width)
    {
        if (touch.py > y && touch.py < y + height)
        {
            return true;
        }
    }
    return false;
}
SwkbdButton keyboard(const char *text_display)
{
    static SwkbdState swkbd;
    SwkbdButton button = SWKBD_BUTTON_NONE;
    swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 1, -1);
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_FILTER_DIGITS | SWKBD_FILTER_AT | SWKBD_FILTER_PERCENT | SWKBD_FILTER_BACKSLASH | SWKBD_FILTER_PROFANITY, 2);
    swkbdSetFeatures(&swkbd, SWKBD_MULTILINE);
    swkbdSetHintText(&swkbd, text_display);
    button = swkbdInputText(&swkbd, text, sizeof(text));
    return button;
}
void update_timer(s64 *timer, s64 *target)
{
    if (*timer > *target)
        --*timer;
    else if (*timer < *target)
        ++*timer;
}
void update_all_timer(void)
{
    update_timer(&timer0, &timer0_target);
    update_timer(&timer1, &timer1_target);
    update_timer(&timer2, &timer2_target);
}
u32 max(u32 a, u32 b)
{
    if (a > b)
        return a;
    if (b > a)
        return b;
    else
        return a;
}
void process_title_anim(void);
void init_title(void);
void update_title(void);
void init_menu_options(void);
void update_menu_options(void);
void init_menu_gamemode(void);
void update_menu_gamemode(void);
int main()
{
    gfxInitDefault();
    cfguInit();
    romfsInit();
    ndspInit();
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    create_audio_thread();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    CFGU_GetSystemLanguage(&language);
    CFGU_IsNFCSupported(&enable_nfc);
    if (enable_nfc)
        nfcInit(NFC_OpType_RawNFC);
    timer0 = 60;
    timer0_target = 0;
    init_title();
    end = false;
    while (aptMainLoop() && !end)
    {
        hidScanInput();
        touchRead(&touch);
        keyDown = hidKeysDown();
        keyHeld = hidKeysHeld();
        update_all_timer();
        if (state == 0)
            update_title();
        else if (state == 1)
            update_menu_options();
        else if (state == 2)
            update_menu_gamemode();
        // if (keyDown & KEY_A)
        //     play_vorbis("romfs:/audio/sfx1.ogg");
        gspWaitForVBlank();
    }

    // Exit services
    if (decoder.plug != 0)
    {
        ModPlug_Unload(decoder.plug);
        linearFree(wavebufs[0].data_pcm16);
        linearFree(wavebufs[1].data_pcm16);
    }
    romfsExit();
    ndspExit();
    gfxExit();
    if (enable_nfc)
        nfcExit();
    return 0;
}
void init_title(void)
{
    state = 0;
    gspWaitForVBlank();
    play_mod("romfs:/audio/title.it");
    C2D_SpriteSheet title_sheet = C2D_SpriteSheetLoad("romfs:/gfx/title.t3x");
    C2D_SpriteSheet title_options_sheet = C2D_SpriteSheetLoad("romfs:/gfx/title_options.t3x");
    if (language == CFG_LANGUAGE_ZH)
    {
        temp0 = 2;
    }
    else if (language == CFG_LANGUAGE_JP)
    {
        temp0 = 1;
    }
    else
        temp0 = 0;
    title_image = C2D_SpriteSheetGetImage(title_sheet, temp0);
    title_options = C2D_SpriteSheetGetImage(title_options_sheet, 0);
    top_render_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottom_render_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    style = 1;
    if (style == 1)
    {
        C2D_SpriteSheet style_bg_sheet = C2D_SpriteSheetLoad("romfs:/gfx/style_1_bg.t3x");
        style_bg = C2D_SpriteSheetGetImage(style_bg_sheet, 0);
    }
    // consoleInit(GFX_BOTTOM, NULL);
    timer1_target = timer1 = 32;
    timer2_target = timer2 = 32;
    disable_title_bump = false;
    gspWaitForVBlank();
}
void update_title(void)
{
    // printf("%lld\n", (timer0 * 255) / 120);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top_render_target, C2D_Color32(47, 218, 255, 255));
    C2D_SceneBegin(top_render_target);
    if (style == 1)
        C2D_DrawImageAt(style_bg, 0, 0, 0, NULL, 1.0f, 1.0f);
    // C2D_Fade(C2D_Color32(timer0 * (47 / 60), timer0 * (218 / 60), timer0 * (255 / 60), timer0 * (255 / 60)));
    C2D_Fade(C2D_Color32(47, 218, 255, timer0 * (255 / 60)));
    if (language == CFG_LANGUAGE_JP)
        C2D_DrawImageAtRotated(title_image, 72 + 100, 66 + 60 + title_bump, 1, C3D_AngleFromDegrees(title_bump / 3.0f), NULL, 1.0f, 1.0f);
    else
        C2D_DrawImageAtRotated(title_image, 97 + 106, 28 + 90 + title_bump, 1, C3D_AngleFromDegrees(title_bump / 3.0f), NULL, 1.0f, 1.0f);

    C2D_SceneBegin(bottom_render_target);
    C2D_TargetClear(bottom_render_target, C2D_Color32(47, 218, 255, 255));
    C2D_DrawImageAt(title_options, (320 - 144) / 2 - (((timer2 - 32) + (32 - timer1)) * (320 / 32)), (240 - 127) / 2, 0, NULL, 1.0f, 1.0f);
    // C2D_Fade(C2D_Color32(timer0 * (47 / 60), timer0 * (218 / 60), timer0 * (255 / 60), timer0 * (255 / 60)));
    C2D_Fade(C2D_Color32(47, 218, 255, timer0 * (255 / 60)));
    C3D_FrameEnd(0);
    if (timer0_target != 60 && check_touched(121, 167, 79, 25))
    {
        timer0_target = 60;
        play_vorbis("romfs:/audio/sfx1.ogg");
    }
    if (timer0 == 60 && timer0_target == 60)
        end = true;
    if (timer1_target != 1 && check_touched(88, 105, 144, 24))
    {
        disable_title_bump = true;
        timer1 = 32;
        timer1_target = 1;
        play_vorbis("romfs:/audio/sfx1.ogg");
    }
    if (timer2_target != 1 && check_touched(111, 56, 99, 24))
    {
        timer2 = 32;
        timer2_target = 1;
        play_vorbis("romfs:/audio/sfx1.ogg");
    }
    if (timer1 == 1 && timer1_target == 1)
    {
        gspWaitForVBlank();
        init_menu_options();
    }
    if (timer2 == 1 && timer2_target == 1)
    {
        gspWaitForVBlank();
        init_menu_gamemode();
    }
    process_title_anim();
    ModPlug_SetMasterVolume(decoder.plug, timer1 * 16);
}
void init_menu_options(void)
{
    state = 1;
    timer1_target = timer1 = 128;
    play_mod("romfs:/audio/options.it");
    ModPlug_SetMasterVolume(decoder.plug, 64);
    disable_title_bump = false;
    title_bump = 0;
    style_anim_timer0 = 0;
    gspWaitForVBlank();
}
void update_menu_options(void)
{
    if (keyDown & KEY_B)
        init_title();
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top_render_target, C2D_Color32(47, 218, 255, 255));
    C2D_SceneBegin(top_render_target);
    if (style == 1)
        C2D_DrawImageAt(style_bg, 0, 0, 1, NULL, 1.0f, 1.0f);
    title_bump += style_anim_timer0 / 20.5f;
    if (title_bump >= 15)
        style_anim_timer0 = -30;
    ++style_anim_timer0;

    if (language == CFG_LANGUAGE_JP)
        C2D_DrawImageAt(title_image, 72 - 41, 70 + title_bump, 1, NULL, 1.0f, 1.0f);
    else if (language == CFG_LANGUAGE_ZH)
        C2D_DrawImageAt(title_image, 97 + 11, 28 + title_bump, 1, NULL, 1.0f, 1.0f);
    else
        C2D_DrawImageAt(title_image, 97, 28 + title_bump, 1, NULL, 1.0f, 1.0f);
    C3D_FrameEnd(0);
}
void init_menu_gamemode(void)
{
    state = 2;
    gspWaitForVBlank();
}
void update_menu_gamemode(void)
{
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top_render_target, C2D_Color32(47, 218, 255, 255));
    C2D_SceneBegin(top_render_target);
    if (style == 1)
        C2D_DrawImageAt(style_bg, 0, 0, 0, NULL, 1.0f, 1.0f);
    if (language == CFG_LANGUAGE_JP)
        C2D_DrawImageAtRotated(title_image, 72 + 100, 66 + 60 + title_bump, 1, C3D_AngleFromDegrees(title_bump / 3.0f), NULL, 1.0f, 1.0f);
    else
        C2D_DrawImageAtRotated(title_image, 97 + 106, 28 + 90 + title_bump, 1, C3D_AngleFromDegrees(title_bump / 3.0f), NULL, 1.0f, 1.0f);

    C2D_SceneBegin(bottom_render_target);
    C2D_TargetClear(bottom_render_target, C2D_Color32(47, 218, 255, 255));
    // C2D_DrawImageAt(title_options, (320 - 144) / 2 - (((timer2 - 32) + (32 - timer1)) * (320 / 32)), (240 - 127) / 2, 0, NULL, 1.0f, 1.0f);
    C3D_FrameEnd(0);
    process_title_anim();
}
void process_title_anim(void)
{
    if (keyDown & KEY_B)
        init_title();
    if (!disable_title_bump)
    {
        if (!title_bump_up)
        {
            if (title_bump <= -15.2f)
                title_bump_up = true;
            title_bump += (-16.0f - title_bump) / 20.0f;
        }
        else
        {
            if (title_bump >= 15.2f)
                title_bump_up = false;
            title_bump += (16.0f - title_bump) / 20.0f;
        }
    }
    else
    {
        temp1 = C3D_AngleFromDegrees(title_bump / 3.0f);
        //((a)*M_TAU/360.0f)
        temp1 += (0 - temp1) / 50;
        temp1 /= M_TAU;
        temp1 *= 360.0f;
        title_bump = temp1;
    }
}