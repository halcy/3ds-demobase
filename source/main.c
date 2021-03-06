#include "Tools.h"
#include "music_bin.h"
#include "Rocket/sync.h"

#include "Effects.h"

#define CLEAR_COLOR 0x555555FF

C3D_Tex fade_tex;
static Pixel* fadePixels;
static Bitmap fadeBitmap;
float fadeVal;

#define min(a, b) (((a)<(b))?(a):(b))
#define max(a, b) (((a)>(b))?(a):(b))

#define AUDIO_BUFSIZE 1024

#define SONG_BPM 110.0
#define SONG_BPS (SONG_BPM / 60.0)
#define SONG_SPS 32000
#define SONG_SPB (SONG_SPS / SONG_BPS)

#define ROWS_PER_BEAT 8
#define SAMPLES_PER_ROW (SONG_SPB / ROWS_PER_BEAT)

int32_t sample_pos = 0;
ndspWaveBuf wave_buffer[2];
uint8_t fill_buffer = 0;
uint8_t audio_playing = 1;

double audio_get_row() {
    return (double)sample_pos / (double)SAMPLES_PER_ROW;
}

#ifndef SYNC_PLAYER
void audio_pause(void *ignored, int flag) {
   ignored;
   audio_playing = !flag;
}

void audio_set_row(void *ignored, int row) {
    ignored;
    sample_pos = row * SAMPLES_PER_ROW - AUDIO_BUFSIZE;
}

int audio_is_playing(void *d) {
    return audio_playing;
}

struct sync_cb rocket_callbakcks = {
    audio_pause,
    audio_set_row,
    audio_is_playing
};
#endif

#define ROCKET_HOST "172.20.10.7"
#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x100000

static uint32_t *SOC_buffer = NULL;

int connect_rocket() {
#ifndef SYNC_PLAYER
    while(sync_tcp_connect(rocket, ROCKET_HOST, SYNC_DEFAULT_PORT)) {
        printf("Didn't work, again...\n");
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) {
            return(1);
        }
        svcSleepThread(1000*1000*1000);
    }
#endif
    return(0);
}

void audio_callback(void* ignored) {
    ignored;
    if(wave_buffer[fill_buffer].status == NDSP_WBUF_DONE && (sample_pos + AUDIO_BUFSIZE) * sizeof(int16_t) < music_bin_size) {
        if(audio_playing == 1) {
            sample_pos += AUDIO_BUFSIZE;
        }
        uint8_t *dest = (uint8_t*)wave_buffer[fill_buffer].data_pcm16;
        memcpy(dest, &music_bin[(sample_pos - AUDIO_BUFSIZE) * sizeof(int16_t)], AUDIO_BUFSIZE * sizeof(int16_t));
        DSP_FlushDataCache(dest, AUDIO_BUFSIZE * sizeof(int16_t));
        ndspChnWaveBufAdd(0, &wave_buffer[fill_buffer]);
        fill_buffer = !fill_buffer;
    }
}

int main() {
    bool DUMPFRAMES = false;
    
    // Initialize graphics
    gfxInit(GSP_RGBA8_OES, GSP_BGR8_OES, false);
    gfxSet3D(true);
    consoleInit(GFX_BOTTOM, NULL);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    // Initialize the render target
    C3D_RenderTarget* targetLeft = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTarget* targetRight = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetClear(targetLeft, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
    C3D_RenderTargetSetClear(targetRight, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
    C3D_RenderTargetSetOutput(targetLeft, GFX_TOP, GFX_LEFT,  DISPLAY_TRANSFER_FLAGS);
    C3D_RenderTargetSetOutput(targetRight, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);

    fadePixels = (Pixel*)linearAlloc(SCREEN_TEXTURE_WIDTH * SCREEN_TEXTURE_HEIGHT * sizeof(Pixel));
    InitialiseBitmap(&fadeBitmap, SCREEN_TEXTURE_WIDTH, SCREEN_TEXTURE_HEIGHT, BytesPerRowForWidth(SCREEN_TEXTURE_WIDTH), fadePixels);
    C3D_TexInit(&fade_tex, SCREEN_TEXTURE_HEIGHT, SCREEN_TEXTURE_WIDTH, GPU_RGBA8);

    romfsInit();
    
    // Rocket startup
#ifndef SYNC_PLAYER
    printf("Now socketing...\n");
    SOC_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    socInit(SOC_buffer, SOC_BUFFERSIZE);
    
    rocket = sync_create_device("sdmc:/sync");
#else
    printf("Loading tracks from romfs...");
    rocket = sync_create_device("romfs:/sync");
#endif
    if(connect_rocket()) {
        return(0);
    }
    
    // Sound on
    ndspInit();
    
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, SONG_SPS);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
    
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0;
    mix[1] = 1.0;    
    ndspChnSetMix(0, mix);

    uint8_t *audio_buffer = (uint8_t*)linearAlloc(AUDIO_BUFSIZE * sizeof(int16_t) * 2);
    memset(wave_buffer,0,sizeof(wave_buffer));
    wave_buffer[0].data_vaddr = &audio_buffer[0];
    wave_buffer[0].nsamples = AUDIO_BUFSIZE;
    wave_buffer[1].data_vaddr = &audio_buffer[AUDIO_BUFSIZE * sizeof(int16_t)];
    wave_buffer[1].nsamples = AUDIO_BUFSIZE;
    
    // Play music
    ndspSetCallback(&audio_callback, 0);
    ndspChnWaveBufAdd(0, &wave_buffer[0]);
    ndspChnWaveBufAdd(0, &wave_buffer[1]);
    
    // Start up first effect
    effectScrollerInit();
    
    const struct sync_track* sync_fade = sync_get_track(rocket, "global.fade");;
    
    int fc = 0;
    while (aptMainLoop()) {
        double row = 0.0;
        if(!DUMPFRAMES) {
            row = audio_get_row();
        }
        else {
            row = ((double)fc * (32000.0 / 30.0)) / (double)SAMPLES_PER_ROW;
        }
        
#ifndef SYNC_PLAYER
        if(sync_update(rocket, (int)floor(row), &rocket_callbakcks, (void *)0)) {
            printf("Lost connection, retrying.\n");
            if(connect_rocket()) {
                return(0);
            }
        }
#endif

        fadeVal = sync_get_val(sync_fade, row);
        
        FillBitmap(&fadeBitmap, RGBAf(1.0, 1.0, 1.0, fadeVal));
        GSPGPU_FlushDataCache(fadePixels, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(Pixel));
        GX_DisplayTransfer((u32*)fadePixels, GX_BUFFER_DIM(SCREEN_TEXTURE_WIDTH, SCREEN_TEXTURE_HEIGHT), (u32*)fade_tex.data, GX_BUFFER_DIM(SCREEN_TEXTURE_WIDTH, SCREEN_TEXTURE_HEIGHT), TEXTURE_TRANSFER_FLAGS);
        gspWaitForPPF();
        
        hidScanInput();
        
        // Respond to user input
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) {
            break; // break in order to return to hbmenu
        }  
        float slider = osGet3DSliderState();
        float iod = slider / 3.0;
        
        effectScrollerRender(targetLeft, targetRight, iod, row);
        
        gspWaitForP3D();
        gspWaitForPPF();
        
        if(DUMPFRAMES) {
            u8* fbl = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
            
            char fname[255];
            sprintf(fname, "fb_left_%08d.raw", fc);
            
            FILE* file = fopen(fname,"w");
            fwrite(fbl, sizeof(int32_t), SCREEN_HEIGHT * SCREEN_WIDTH, file);
            fflush(file);
            fclose(file);
        }
        
        fc++;   
    }
    
    linearFree(fadePixels);
    
    // Sound off
    ndspExit();
    linearFree(audio_buffer);
    
    // Deinitialize graphics
    socExit();
    C3D_Fini();
    gfxExit();
    return 0;
}
