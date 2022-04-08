#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/avstring.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

/**
 * Prevents SDL from overriding main().
 */
#ifdef __MINGW32__
#undef main
#endif

/**
 * Debug flag.
 */
#define _DEBUG_ 1

/**
 * SDL audio buffer size in samples.
 */
#define SDL_AUDIO_BUFFER_SIZE 1024

/**
 * Maximum number of samples per channel in an audio frame.
 */
#define MAX_AUDIO_FRAME_SIZE 192000

/**
 * Audio packets queue maximum size.
 */
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)


/**
 * No AV sync correction threshold.
 */
#define AV_NOSYNC_THRESHOLD 1.0

/**
 *
 */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/**
 *
 */
#define AUDIO_DIFF_AVG_NB 20

/**
 * Custom SDL_Event type.
 * Notifies the next video frame has to be displayed.
 */
#define FF_REFRESH_EVENT (SDL_USEREVENT)

/**
 * Custom SDL_Event type.
 * Notifies the program needs to quit.
 */
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

/**
 * Default audio video sync type.
 */
#define DEFAULT_AV_SYNC_TYPE AV_SYNC_AUDIO_MASTER

/**
 * Queue structure used to store AVPackets.
 */
typedef struct PacketQueue
{
    AVPacketList *  first_pkt;
    AVPacketList *  last_pkt;
    int             nb_packets;
    int             size;
    SDL_mutex *     mutex;
    SDL_cond *      cond;
} PacketQueue;


/**
 * Struct used to hold the format context, the indices of the audio and video stream,
 * the corresponding AVStream objects, the audio and video codec information,
 * the audio and video queues and buffers, the global quit flag and the filename of
 * the movie.
 */
typedef struct VideoState
{
    /**
     * File I/O Context.
     */
    AVFormatContext * pFormatCtx;

    /**
     * Audio Stream.
     */
    int                 audioStream;
    AVStream *          audio_st;
    AVCodecContext *    audio_ctx;
    PacketQueue         audioq;
    uint8_t             audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) /2];
    unsigned int        audio_buf_size;
    unsigned int        audio_buf_index;
    AVPacket            audio_pkt;
    double              audio_clock;
    double              audio_diff_cum;
    double              audio_diff_avg_coef;
    double              audio_diff_threshold;
    int                 audio_diff_avg_count;

    /**
     * AV Sync.
     */
    int     av_sync_type;
    double  external_clock;
    int64_t external_clock_time;

    /**
     * Seeking.
     */
    int     seek_req;
    int     seek_flags;
    int64_t seek_pos;

    /**
     * Threads.
     */
    SDL_Thread *    decode_tid;

    /**
     * Input file name.
     */
    char filename[1024];

    /**
     * Global quit flag.
     */
    int quit;

//    /**
//     * Maximum number of frames to be decoded.
//     */
    long    maxFramesToDecode;
} VideoState;

/**
 * Struct used to hold data fields used for audio resampling.
 */
typedef struct AudioResamplingState
{
    SwrContext * swr_ctx;
    int64_t in_channel_layout;
    uint64_t out_channel_layout;
    int out_nb_channels;
    int out_linesize;
    int in_nb_samples;
    int64_t out_nb_samples;
    int64_t max_out_nb_samples;
    uint8_t ** resampled_data;
    int resampled_data_size;

} AudioResamplingState;

/**
 * Audio Video Sync Types.
 */
enum
{
    /**
     * Sync to audio clock.
     */
    AV_SYNC_AUDIO_MASTER,

    /**
     * Sync to external clock: the computer clock
     */
    AV_SYNC_EXTERNAL_MASTER,
};

/**
 * Global VideoState reference.
 */
VideoState * global_video_state;

/**
 *
 */
AVPacket flush_pkt;

/**
 * Methods declaration.
 */
void printHelpMenu();

int decode_thread(void * arg);

int stream_component_open(
        VideoState * videoState,
        int stream_index
);

int synchronize_audio(
        VideoState * videoState,
        short * samples,
        int samples_size
);


double get_audio_clock(VideoState * videoState);

double get_external_clock(VideoState * videoState);

double get_master_clock(VideoState * videoState);

static void schedule_refresh(
        VideoState * videoState,
        Uint32 delay
);

static Uint32 sdl_refresh_timer_cb(
        Uint32 interval,
        void * param
);

void packet_queue_init(PacketQueue * q);

int packet_queue_put(
        PacketQueue * queue,
        AVPacket * packet
);

static int packet_queue_get(
        PacketQueue * queue,
        AVPacket * packet,
        int blocking
);

static void packet_queue_flush(PacketQueue * queue);

void audio_callback(
        void * userdata,
        Uint8 * stream,
        int len
);

int audio_decode_frame(
        VideoState * videoState,
        uint8_t * audio_buf,
        int buf_size,
        double * pts_ptr
);

static int audio_resampling(
        VideoState * videoState,
        AVFrame * decoded_audio_frame,
        enum AVSampleFormat out_sample_fmt,
        uint8_t * out_buf
);

AudioResamplingState * getAudioResampling(uint64_t channel_layout);

void stream_seek(VideoState * videoState, int64_t pos, int rel);

/**
 * Entry point.
 *
 * @param   argc    command line arguments counter.
 * @param   argv    command line arguments.
 *
 * @return          execution exit code.
 */
int main(int argc, char * argv[])
{
    // if the given number of command line arguments is wrong
    if ( argc != 3 )
    {
        // print help menu and exit
        printHelpMenu();
        return -1;
    }

    /**
     * Initialize SDL.
     * New API: this implementation does not use deprecated SDL functionalities.
     */
    int ret = SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER);
    if (ret != 0)
    {
        printf("Could not initialize SDL - %s\n.", SDL_GetError());
        return -1;
    }

    // the global VideoState reference will be set in decode_thread() to this pointer
    VideoState * videoState = NULL;

    // allocate memory for the VideoState and zero it out
    videoState = av_mallocz(sizeof(VideoState));

    // copy the file name input by the user to the VideoState structure
    av_strlcpy(videoState->filename, argv[1], sizeof(videoState->filename));

    // parse max frames to decode input by the user
    char * pEnd;
    videoState->maxFramesToDecode = strtol(argv[2], &pEnd, 10);

    // launch our threads by pushing an SDL_event of type FF_REFRESH_EVENT
    schedule_refresh(videoState, 100);

    videoState->av_sync_type = DEFAULT_AV_SYNC_TYPE;

    // start the decoding thread to read data from the AVFormatContext
    videoState->decode_tid = SDL_CreateThread(decode_thread, "Decoding Thread", videoState);

    // check the decode thread was correctly started
    if(!videoState->decode_tid)
    {
        printf("Could not start decoding SDL_Thread: %s.\n", SDL_GetError());

        // free allocated memory before exiting
        av_free(videoState);

        return -1;
    }

    av_init_packet(&flush_pkt);
    flush_pkt.data = "FLUSH";

    // infinite loop waiting for fired events
    SDL_Event event;
    for(;;)
    {
        double incr, pos;

        // wait indefinitely for the next available event
        ret = SDL_WaitEvent(&event);
        if (ret == 0)
        {
            printf("SDL_WaitEvent failed: %s.\n", SDL_GetError());
        }

        // switch on the retrieved event type
        switch(event.type)
        {
            case SDL_KEYDOWN:
            {
                switch(event.key.keysym.sym)
                {
                    case SDLK_LEFT:
                    {
                        incr = -10.0;
                        goto do_seek;
                    }
                    case SDLK_RIGHT:
                    {
                        incr = 10.0;
                        goto do_seek;
                    }
                    case SDLK_DOWN:
                    {
                        incr = -60.0;
                        goto do_seek;
                    }
                    case SDLK_UP:
                    {
                        incr = 60.0;
                        goto do_seek;
                    }

                    do_seek:
                    {
                        if(global_video_state)
                        {
                            pos = get_master_clock(global_video_state);
                            pos += incr;
                            stream_seek(global_video_state, (int64_t)(pos * AV_TIME_BASE), incr);
                        }
                        break;
                    };

                    default:
                    {
                        // nothing to do
                    }
                        break;
                }
            }
                break;

            case FF_QUIT_EVENT:
            case SDL_QUIT:
            {
                videoState->quit = 1;

                /**
                 * If the video has finished playing, then both the picture and audio
                 * queues are waiting for more data.  Make them stop waiting and
                 * terminate normally.
                 */
                SDL_CondSignal(videoState->audioq.cond);

                SDL_Quit();
            }
                break;

            default:
            {
                // nothing to do
            }
                break;
        }

        // check global quit flag
        if (videoState->quit)
        {
            // exit for loop
            break;
        }
    }

    // clean up memory
    av_free(videoState);

    return 0;
}

/**
 * Print help menu containing usage information.
 */
void printHelpMenu()
{
    printf("Invalid arguments.\n\n");
    printf("Usage: ./tutorial08 <filename> <max-frames-to-decode>\n\n");
}

/**
 * This function is used as callback for the SDL_Thread.
 *
 * Opens Audio and Video Streams. If all codecs are retrieved correctly, starts
 * an infinite loop to read AVPackets from the global VideoState AVFormatContext.
 * Based on their stream index, each packet is placed in the appropriate queue.
 *
 * @param   arg the data pointer passed to the SDL_Thread callback function.
 *
 * @return      < 0 in case of error, 0 otherwise.
 */
int decode_thread(void * arg)
{
    // retrieve global VideoState reference
    VideoState * videoState = (VideoState *)arg;

    // file I/O context: demuxers read a media file and split it into chunks of data (packets)
    AVFormatContext * pFormatCtx = NULL;
    int ret = avformat_open_input(&pFormatCtx, videoState->filename, NULL, NULL);
    if (ret < 0)
    {
        printf("Could not open file %s.\n", videoState->filename);
        return -1;
    }

    // reset stream indexes
    videoState->audioStream = -1;

    // set global VideoState reference
    global_video_state = videoState;

    // set the AVFormatContext for the global VideoState reference
    videoState->pFormatCtx = pFormatCtx;

    // read packets of the media file to get stream information
    ret = avformat_find_stream_info(pFormatCtx, NULL);
    if (ret < 0)
    {
        printf("Could not find stream information: %s.\n", videoState->filename);
        return -1;
    }

    // dump information about file onto standard error
    if (_DEBUG_)
        av_dump_format(pFormatCtx, 0, videoState->filename, 0);

    // audio stream indexes
    int audioStream = -1;

    // loop through the streams that have been found
    for (int i = 0; i < pFormatCtx->nb_streams; i++)
    {
        // look for the audio stream
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStream < 0)
        {
            audioStream = i;
        }
    }

    // return with error in case no audio stream was found
    if (audioStream == -1)
    {
        printf("Could not find audio stream.\n");
        goto fail;
    }
    else
    {
        // open audio stream component codec
        ret = stream_component_open(videoState, audioStream);

        // check audio codec was opened correctly
        if (ret < 0)
        {
            printf("Could not open audio codec.\n");
            goto fail;
        }
    }


    // alloc the AVPacket used to read the media file
    AVPacket * packet = av_packet_alloc();
    if (packet == NULL)
    {
        printf("Could not allocate AVPacket.\n");
        goto fail;
    }

    // main decode loop: read in a packet and put it on the right queue
    for (;;)
    {
        // check global quit flag
        if (videoState->quit)
        {
            break;
        }

        // seek stuff goes here
        if(videoState->seek_req)
        {
            int audio_stream_index = -1;
            int64_t seek_target = videoState->seek_pos;

            if (videoState->audioStream >= 0)
            {
                audio_stream_index = videoState->audioStream;
            }

            if( audio_stream_index >= 0)
            {

                seek_target = av_rescale_q(seek_target, AV_TIME_BASE_Q, pFormatCtx->streams[audio_stream_index]->time_base);
            }

            ret &= av_seek_frame(videoState->pFormatCtx, audio_stream_index, seek_target, videoState->seek_flags);

            if (ret < 0)
            {
                fprintf(stderr, "%s: error while seeking\n", videoState->filename);
            }
            else
            {

                if (videoState->audioStream >= 0)
                {
                    packet_queue_flush(&videoState->audioq);
                    packet_queue_put(&videoState->audioq, &flush_pkt);
                }
            }

            videoState->seek_req = 0;
        }

        // check audio and video packets queues size
        if (videoState->audioq.size > MAX_AUDIOQ_SIZE)
        {
            // wait for audio and video queues to decrease size
            SDL_Delay(10);

            continue;
        }

        // read data from the AVFormatContext by repeatedly calling av_read_frame()
        ret = av_read_frame(videoState->pFormatCtx, packet);
        if (ret < 0)
        {
            if (ret == AVERROR_EOF)
            {
                // media EOF reached, quit
                videoState->quit = 1;
                break;
            }
            else if (videoState->pFormatCtx->pb->error == 0)
            {
                // no read error; wait for user input
                SDL_Delay(10);

                continue;
            }
            else
            {
                // exit for loop in case of error
                break;
            }
        }

        if (packet->stream_index == videoState->audioStream)
        {
            packet_queue_put(&videoState->audioq, packet);
        }
        else
        {
            // otherwise free the memory
            av_packet_unref(packet);
        }
    }

    // wait for the rest of the program to end
    while (!videoState->quit)
    {
        SDL_Delay(100);
    }

    // close the opened input AVFormatContext
    avformat_close_input(&pFormatCtx);

    // in case of failure, push the FF_QUIT_EVENT and return
    fail:
    {
        // create an SDL_Event of type FF_QUIT_EVENT
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = videoState;

        // push the event to the events queue
        SDL_PushEvent(&event);

        // return with error
        return -1;
    };
}

/**
 * Retrieves the AVCodec and initializes the AVCodecContext for the given AVStream
 * index. In case of AVMEDIA_TYPE_AUDIO codec type, it sets the desired audio specs,
 * opens the audio device and starts playing.
 *
 * @param   videoState      the global VideoState reference used to save info
 *                          related to the media being played.
 * @param   stream_index    the stream index obtained from the AVFormatContext.
 *
 * @return                  < 0 in case of error, 0 otherwise.
 */
int stream_component_open(VideoState * videoState, int stream_index)
{
    // retrieve file I/O context
    AVFormatContext * pFormatCtx = videoState->pFormatCtx;

    // check the given stream index is valid
    if (stream_index < 0 || stream_index >= pFormatCtx->nb_streams)
    {
        printf("Invalid stream index.");
        return -1;
    }

    // retrieve codec for the given stream index
    AVCodec * codec = NULL;
    codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codecpar->codec_id);
    if (codec == NULL)
    {
        printf("Unsupported codec.\n");
        return -1;
    }

    // retrieve codec context
    AVCodecContext * codecCtx = NULL;
    codecCtx = avcodec_alloc_context3(codec);
    int ret = avcodec_parameters_to_context(codecCtx, pFormatCtx->streams[stream_index]->codecpar);
    if (ret != 0)
    {
        printf("Could not copy codec context.\n");
        return -1;
    }

    // in case of Audio codec, set up and open the audio device
    if (codecCtx->codec_type == AVMEDIA_TYPE_AUDIO)
    {
        // desired and obtained audio specs references
        SDL_AudioSpec wanted_specs;
        SDL_AudioSpec specs;

    

        // Set audio settings from codec info
        wanted_specs.freq = codecCtx->sample_rate;
        wanted_specs.format = AUDIO_S16SYS;
        wanted_specs.channels = codecCtx->channels;
        wanted_specs.silence = 0;
        wanted_specs.samples = SDL_AUDIO_BUFFER_SIZE;
        wanted_specs.callback = audio_callback;
        wanted_specs.userdata = videoState;

        /* Deprecated, please refer to tutorial04-resampled.c for the new API */
        // open audio device
        ret = SDL_OpenAudio(&wanted_specs, &specs);

        // check audio device was correctly opened
        if (ret < 0)
        {
            printf("SDL_OpenAudio: %s.\n", SDL_GetError());
            return -1;
        }
    }

    // initialize the AVCodecContext to use the given AVCodec
    if (avcodec_open2(codecCtx, codec, NULL) < 0)
    {
        printf("Unsupported codec.\n");
        return -1;
    }

    // set up the global VideoState based on the type of the codec obtained for
    // the given stream index
    switch (codecCtx->codec_type)
    {
        case AVMEDIA_TYPE_AUDIO:
        {
            // set VideoState audio related fields
            videoState->audioStream = stream_index;
            videoState->audio_st = pFormatCtx->streams[stream_index];
            videoState->audio_ctx = codecCtx;
            videoState->audio_buf_size = 0;
            videoState->audio_buf_index = 0;

            // zero out the block of memory pointed by videoState->audio_pkt
            memset(&videoState->audio_pkt, 0, sizeof(videoState->audio_pkt));

            // init audio packet queue
            packet_queue_init(&videoState->audioq);

            // start playing audio on the first audio device
            SDL_PauseAudio(0);
        }
            break;
        default:
        {
            // nothing to do
        }
            break;
    }

    return 0;
}


/**
 * So we're going to use a fractional coefficient, say c, and So now let's say
 * we've gotten N audio sample sets that have been out of sync. The amount we are
 * out of sync can also vary a good deal, so we're going to take an average of how
 * far each of those have been out of sync. So for example, the first call might
 * have shown we were out of sync by 40ms, the next by 50ms, and so on. But we're
 * not going to take a simple average because the most recent values are more
 * important than the previous ones. So we're going to use a fractional coefficient,
 * say c, and sum the differences like this: diff_sum = new_diff + diff_sum*c.
 * When we are ready to find the average difference, we simply calculate
 * avg_diff = diff_sum * (1-c).
 *
 * @param   videoState      the global VideoState reference.
 * @param   samples         global VideoState reference audio buffer.
 * @param   samples_size    last decoded audio AVFrame size after resampling.
 *
 * @return
 */
int synchronize_audio(VideoState * videoState, short * samples, int samples_size)
{
    int n;
    double ref_clock;

    n = 2 * videoState->audio_ctx->channels;

    // check if
    if (videoState->av_sync_type != AV_SYNC_AUDIO_MASTER)
    {
        double diff, avg_diff;
        int wanted_size, min_size, max_size /*, nb_samples */;

        ref_clock = get_master_clock(videoState);
        diff = get_audio_clock(videoState) - ref_clock;

        if (diff < AV_NOSYNC_THRESHOLD)
        {
            // accumulate the diffs
            videoState->audio_diff_cum = diff + videoState->audio_diff_avg_coef * videoState->audio_diff_cum;

            if (videoState->audio_diff_avg_count < AUDIO_DIFF_AVG_NB)
            {
                videoState->audio_diff_avg_count++;
            }
            else
            {
                avg_diff = videoState->audio_diff_cum * (1.0 - videoState->audio_diff_avg_coef);

                /**
                 * So we're doing pretty well; we know approximately how off the audio
                 * is from the video or whatever we're using for a clock. So let's now
                 * calculate how many samples we need to add or lop off by putting this
                 * code where the "Shrinking/expanding buffer code" section is:
                 */
                if (fabs(avg_diff) >= videoState->audio_diff_threshold)
                {
                    wanted_size = samples_size + ((int)(diff * videoState->audio_ctx->sample_rate) * n);
                    min_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
                    max_size = samples_size * ((100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100);

                    if(wanted_size < min_size)
                    {
                        wanted_size = min_size;
                    }
                    else if (wanted_size > max_size)
                    {
                        wanted_size = max_size;
                    }

                    /**
                     * Now we have to actually correct the audio. You may have noticed that our
                     * synchronize_audio function returns a sample size, which will then tell us
                     * how many bytes to send to the stream. So we just have to adjust the sample
                     * size to the wanted_size. This works for making the sample size smaller.
                     * But if we want to make it bigger, we can't just make the sample size larger
                     * because there's no more data in the buffer! So we have to add it. But what
                     * should we add? It would be foolish to try and extrapolate audio, so let's
                     * just use the audio we already have by padding out the buffer with the
                     * value of the last sample.
                     */
                    if(wanted_size < samples_size)
                    {
                        /* remove samples */
                        samples_size = wanted_size;
                    }
                    else if(wanted_size > samples_size)
                    {
                        uint8_t *samples_end, *q;
                        int nb;

                        /* add samples by copying final sample*/
                        nb = (samples_size - wanted_size);
                        samples_end = (uint8_t *)samples + samples_size - n;
                        q = samples_end + n;

                        while(nb > 0)
                        {
                            memcpy(q, samples_end, n);
                            q += n;
                            nb -= n;
                        }

                        samples_size = wanted_size;
                    }
                }
            }
        }
        else
        {
            /* difference is TOO big; reset diff stuff */
            videoState->audio_diff_avg_count = 0;
            videoState->audio_diff_cum = 0;
        }
    }

    return samples_size;
}


/**
 * Calculates and returns the current audio clock reference value.
 *
 * @param   videoState  the global VideoState reference.
 *
 * @return              the current audio clock reference value.
 */
double get_audio_clock(VideoState * videoState)
{
    double pts = videoState->audio_clock;

    int hw_buf_size = videoState->audio_buf_size - videoState->audio_buf_index;

    int bytes_per_sec = 0;

    int n = 2 * videoState->audio_ctx->channels;

    if (videoState->audio_st)
    {
        bytes_per_sec = videoState->audio_ctx->sample_rate * n;
    }

    if (bytes_per_sec)
    {
        pts -= (double) hw_buf_size / bytes_per_sec;
    }

    return pts;
}

/**
 * Calculates and returns the current external clock reference value: the computer
 * clock.
 *
 * @return  the current external clock reference value.
 */
double get_external_clock(VideoState * videoState)
{
    videoState->external_clock_time = av_gettime();
    videoState->external_clock = videoState->external_clock_time / 1000000.0;

    return videoState->external_clock;
}

/**
 * Checks the VideoState global reference av_sync_type variable and then calls
 * get_audio_clock, get_video_clock, or get_external_clock accordingly.
 *
 * @param   videoState  the global VideoState reference.
 *
 * @return              the reference clock according to the chosen AV sync type.
 */
double get_master_clock(VideoState * videoState)
{
 if (videoState->av_sync_type == AV_SYNC_AUDIO_MASTER)
    {
        return get_audio_clock(videoState);
    }
    else if (videoState->av_sync_type == AV_SYNC_EXTERNAL_MASTER)
    {
        return get_external_clock(videoState);
    }
    else
    {
        fprintf(stderr, "Error: Undefined A/V sync type.");
        return -1;
    }
}

/**
 * Schedules video updates - every time we call this function, it will set the
 * timer, which will trigger an event, which will have our main() function in turn
 * call a function that pulls a frame from our picture queue and displays it.
 *
 * @param   videoState  the global VideoState reference.
 * @param   delay       the delay, expressed in milliseconds, before displaying
 *                      the next video frame on the screen.
 */
static void schedule_refresh(VideoState * videoState, Uint32 delay)
{
    // schedule an SDL timer
    int ret = SDL_AddTimer(delay, sdl_refresh_timer_cb, videoState);

    // check the timer was correctly scheduled
    if (ret == 0)
    {
        printf("Could not schedule refresh callback: %s.\n.", SDL_GetError());
    }
}

/**
 * This is the callback function for the SDL Timer.
 *
 * Pushes an SDL_Event of type FF_REFRESH_EVENT to the events queue.
 *
 * @param   interval    the timer delay in milliseconds.
 * @param   param       user defined data passed to the callback function when
 *                      scheduling the timer. In our case the global VideoState
 *                      reference.
 *
 * @return              if the returned value from the callback is 0, the timer
 *                      is canceled.
 */
static Uint32 sdl_refresh_timer_cb(Uint32 interval, void * param)
{
    // create an SDL_Event of type FF_REFRESH_EVENT
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = param;

    // push the event to the events queue
    SDL_PushEvent(&event);

    // return 0 to cancel the timer
    return 0;
}

/**
 * Initialize the given PacketQueue.
 *
 * @param q the PacketQueue to be initialized.
 */
void packet_queue_init(PacketQueue * q)
{
    // alloc memory for the audio queue
    memset(
            q,
            0,
            sizeof(PacketQueue)
    );

    // Returns the initialized and unlocked mutex or NULL on failure
    q->mutex = SDL_CreateMutex();
    if (!q->mutex)
    {
        // could not create mutex
        printf("SDL_CreateMutex Error: %s.\n", SDL_GetError());
        return;
    }

    // Returns a new condition variable or NULL on failure
    q->cond = SDL_CreateCond();
    if (!q->cond)
    {
        // could not create condition variable
        printf("SDL_CreateCond Error: %s.\n", SDL_GetError());
        return;
    }
}

/**
 * Put the given AVPacket in the given PacketQueue.
 *
 * @param  queue    the queue to be used for the insert
 * @param  packet   the AVPacket to be inserted in the queue
 *
 * @return          0 if the AVPacket is correctly inserted in the given PacketQueue.
 */
int packet_queue_put(PacketQueue * queue, AVPacket * packet)
{
    // alloc the new AVPacketList to be inserted in the audio PacketQueue
    AVPacketList * avPacketList;
    avPacketList = av_malloc(sizeof(AVPacketList));

    // check the AVPacketList was allocated
    if (!avPacketList)
    {
        return -1;
    }

    // add reference to the given AVPacket
    avPacketList->pkt = * packet;

    // the new AVPacketList will be inserted at the end of the queue
    avPacketList->next = NULL;

    // lock mutex
    SDL_LockMutex(queue->mutex);

    // check the queue is empty
    if (!queue->last_pkt)
    {
        // if it is, insert as first
        queue->first_pkt = avPacketList;
    }
    else
    {
        // if not, insert as last
        queue->last_pkt->next = avPacketList;
    }

    // point the last AVPacketList in the queue to the newly created AVPacketList
    queue->last_pkt = avPacketList;

    // increase by 1 the number of AVPackets in the queue
    queue->nb_packets++;

    // increase queue size by adding the size of the newly inserted AVPacket
    queue->size += avPacketList->pkt.size;

    // notify packet_queue_get which is waiting that a new packet is available
    SDL_CondSignal(queue->cond);

    // unlock mutex
    SDL_UnlockMutex(queue->mutex);

    return 0;
}

/**
 * Get the first AVPacket from the given PacketQueue.
 *
 * @param   queue       the PacketQueue to extract from.
 * @param   packet      the first AVPacket extracted from the queue.
 * @param   blocking    0 to avoid waiting for an AVPacket to be inserted in the given
 *                      queue, != 0 otherwise.
 *
 * @return              < 0 if returning because the quit flag is set, 0 if the queue
 *                      is empty, 1 if it is not empty and a packet was extracted.
 */
static int packet_queue_get(PacketQueue * queue, AVPacket * packet, int blocking)
{
    int ret;

    AVPacketList * avPacketList;

    // lock mutex
    SDL_LockMutex(queue->mutex);

    for (;;)
    {
        // check quit flag
        if (global_video_state->quit)
        {
            ret = -1;
            break;
        }

        // point to the first AVPacketList in the queue
        avPacketList = queue->first_pkt;

        // if the first packet is not NULL, the queue is not empty
        if (avPacketList)
        {
            // place the second packet in the queue at first position
            queue->first_pkt = avPacketList->next;

            // check if queue is empty after removal
            if (!queue->first_pkt)
            {
                // first_pkt = last_pkt = NULL = empty queue
                queue->last_pkt = NULL;
            }

            // decrease the number of packets in the queue
            queue->nb_packets--;

            // decrease the size of the packets in the queue
            queue->size -= avPacketList->pkt.size;

            // point packet to the extracted packet, this will return to the calling function
            *packet = avPacketList->pkt;

            // free memory
            av_free(avPacketList);

            ret = 1;
            break;
        }
        else if (!blocking)
        {
            ret = 0;
            break;
        }
        else
        {
            // unlock mutex and wait for cond signal, then lock mutex again
            SDL_CondWait(queue->cond, queue->mutex);
        }
    }

    // unlock mutex
    SDL_UnlockMutex(queue->mutex);

    return ret;
}

/**
 *
 * @param queue
 */
static void packet_queue_flush(PacketQueue * queue)
{
    AVPacketList *pkt, *pkt1;

    SDL_LockMutex(queue->mutex);

    for (pkt = queue->first_pkt; pkt != NULL; pkt = pkt1)
    {
        pkt1 = pkt->next;
        // av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }

    queue->last_pkt = NULL;
    queue->first_pkt = NULL;
    queue->nb_packets = 0;
    queue->size = 0;

    SDL_UnlockMutex(queue->mutex);
}

/**
 * Pull in data from audio_decode_frame(), store the result in an intermediary
 * buffer, attempt to write as many bytes as the amount defined by len to
 * stream, and get more data if we don't have enough yet, or save it for later
 * if we have some left over.
 *
 * @param   userdata    the pointer we gave to SDL.
 * @param   stream      the buffer we will be writing audio data to.
 * @param   len         the size of that buffer.
 */
void audio_callback(void * userdata, Uint8 * stream, int len)
{
    // retrieve the VideoState
    VideoState * videoState = (VideoState *)userdata;

    double pts;

    // while the length of the audio data buffer is > 0
    while (len > 0)
    {
        // check global quit flag
        if (global_video_state->quit)
        {
            return;
        }

        // check how much audio is left to writes
        if (videoState->audio_buf_index >= videoState->audio_buf_size)
        {
            // we have already sent all avaialble data; get more
            int audio_size = audio_decode_frame(
                    videoState,
                    videoState->audio_buf,
                    sizeof(videoState->audio_buf),
                    &pts
            );

            // if error
            if (audio_size < 0)
            {
                // output silence
                videoState->audio_buf_size = 1024;

                // clear memory
                memset(videoState->audio_buf, 0, videoState->audio_buf_size);

                printf("audio_decode_frame() failed.\n");
            }
            else
            {
                audio_size = synchronize_audio(videoState, (int16_t *)videoState->audio_buf, audio_size);

                // cast to usigned just to get rid of annoying warning messages
                videoState->audio_buf_size = (unsigned)audio_size;
            }

            videoState->audio_buf_index = 0;
        }

        int len1 = videoState->audio_buf_size - videoState->audio_buf_index;

        if (len1 > len)
        {
            len1 = len;
        }

        // copy data from audio buffer to the SDL stream
        memcpy(stream, (uint8_t *)videoState->audio_buf + videoState->audio_buf_index, len1);

        len -= len1;
        stream += len1;

        // update global VideoState audio buffer index
        videoState->audio_buf_index += len1;
    }
}

/**
 * Get a packet from the queue if available. Decode the extracted packet. Once
 * we have the frame, resample it and simply copy it to our audio buffer, making
 * sure the data_size is smaller than our audio buffer.
 *
 * @param   aCodecCtx   the audio AVCodecContext used for decoding
 * @param   audio_buf   the audio buffer to write into
 * @param   buf_size    the size of the audio buffer, 1.5 larger than the one
 *                      provided by FFmpeg
 * @param   pts_ptr     a pointer to the pts of the decoded audio frame.
 *
 * @return              0 if everything goes well, -1 in case of error or quit
 */
int audio_decode_frame(VideoState * videoState, uint8_t * audio_buf, int buf_size, double * pts_ptr)
{
    // allocate AVPacket to read from the audio PacketQueue (audioq)
    AVPacket * avPacket = av_packet_alloc();
    if (avPacket == NULL)
    {
        printf("Could not allocate AVPacket.\n");
        return -1;
    }

    static uint8_t * audio_pkt_data = NULL;
    static int audio_pkt_size = 0;

    double pts;
    int n;

    // allocate a new frame, used to decode audio packets
    static AVFrame * avFrame = NULL;
    avFrame = av_frame_alloc();
    if (!avFrame)
    {
        printf("Could not allocate AVFrame.\n");
        return -1;
    }

    int len1 = 0;
    int data_size = 0;

    // infinite loop: read AVPackets from the audio PacketQueue, decode them into
    // audio frames, resample the obtained frame and update the audio buffer
    for (;;)
    {
        // check global quit flag
        if (videoState->quit)
        {
            return -1;
        }

        // check if we obtained an AVPacket from the audio PacketQueue
        while (audio_pkt_size > 0)
        {
            int got_frame = 0;

            // get decoded output data from decoder
            int ret = avcodec_receive_frame(videoState->audio_ctx, avFrame);

            // check and entire audio frame was decoded
            if (ret == 0)
            {
                got_frame = 1;
            }

            // check the decoder needs more AVPackets to be sent
            if (ret == AVERROR(EAGAIN))
            {
                ret = 0;
            }

            if (ret == 0)
            {
                // give the decoder raw compressed data in an AVPacket
                ret = avcodec_send_packet(videoState->audio_ctx, avPacket);
            }

            // check the decoder needs more AVPackets to be sent
            if (ret == AVERROR(EAGAIN))
            {
                ret = 0;
            }
            else if (ret < 0)
            {
                printf("avcodec_receive_frame decoding error.\n");
                return -1;
            }
            else
            {
                len1 = avPacket->size;
            }

            if (len1 < 0)
            {
                // if error, skip frame
                audio_pkt_size = 0;
                break;
            }

            audio_pkt_data += len1;
            audio_pkt_size -= len1;
            data_size = 0;

            // if we decoded an entire audio frame
            if (got_frame)
            {
                // apply audio resampling to the decoded frame
                data_size = audio_resampling(
                        videoState,
                        avFrame,
                        AV_SAMPLE_FMT_S16,
                        audio_buf
                );

                assert(data_size <= buf_size);
            }

            if (data_size <= 0)
            {
                // no data yet, get more frames
                continue;
            }

            // keep audio_clock up-to-date
            pts = videoState->audio_clock;
            *pts_ptr = pts;
            n = 2 * videoState->audio_ctx->channels;
            videoState->audio_clock += (double)data_size / (double)(n * videoState->audio_ctx->sample_rate);

            // we have the data, return it and come back for more later
            return data_size;
        }

        if (avPacket->data)
        {
            // wipe the packet
            av_packet_unref(avPacket);
        }

        // get more audio AVPacket
        int ret = packet_queue_get(&videoState->audioq, avPacket, 1);

        // if packet_queue_get returns < 0, the global quit flag was set
        if (ret < 0)
        {
            return -1;
        }

        if (avPacket->data == flush_pkt.data)
        {
            avcodec_flush_buffers(videoState->audio_ctx);

            continue;
        }

        audio_pkt_data = avPacket->data;
        audio_pkt_size = avPacket->size;

        // keep audio_clock up-to-date
        if (avPacket->pts != AV_NOPTS_VALUE)
        {
            videoState->audio_clock = av_q2d(videoState->audio_st->time_base)*avPacket->pts;
        }
    }

    return 0;
}

/**
 * Resamples the audio data retrieved using FFmpeg before playing it.
 *
 * @param   videoState          the global VideoState reference.
 * @param   decoded_audio_frame the decoded audio frame.
 * @param   out_sample_fmt      audio output sample format (e.g. AV_SAMPLE_FMT_S16).
 * @param   out_buf             audio output buffer.
 *
 * @return                      the size of the resampled audio data.
 */
static int audio_resampling(VideoState * videoState, AVFrame * decoded_audio_frame, enum AVSampleFormat out_sample_fmt, uint8_t * out_buf)
{
    // get an instance of the AudioResamplingState struct
    AudioResamplingState * arState = getAudioResampling(videoState->audio_ctx->channel_layout);

    if (!arState->swr_ctx)
    {
        printf("swr_alloc error.\n");
        return -1;
    }

    // get input audio channels
    arState->in_channel_layout = (videoState->audio_ctx->channels ==
                                  av_get_channel_layout_nb_channels(videoState->audio_ctx->channel_layout)) ?
                                 videoState->audio_ctx->channel_layout :
                                 av_get_default_channel_layout(videoState->audio_ctx->channels);

    // check input audio channels correctly retrieved
    if (arState->in_channel_layout <= 0)
    {
        printf("in_channel_layout error.\n");
        return -1;
    }

    // set output audio channels based on the input audio channels
    if (videoState->audio_ctx->channels == 1)
    {
        arState->out_channel_layout = AV_CH_LAYOUT_MONO;
    }
    else if (videoState->audio_ctx->channels == 2)
    {
        arState->out_channel_layout = AV_CH_LAYOUT_STEREO;
    }
    else
    {
        arState->out_channel_layout = AV_CH_LAYOUT_SURROUND;
    }

    // retrieve number of audio samples (per channel)
    arState->in_nb_samples = decoded_audio_frame->nb_samples;
    if (arState->in_nb_samples <= 0)
    {
        printf("in_nb_samples error.\n");
        return -1;
    }

    // Set SwrContext parameters for resampling
    av_opt_set_int(
            arState->swr_ctx,
            "in_channel_layout",
            arState->in_channel_layout,
            0
    );

    // Set SwrContext parameters for resampling
    av_opt_set_int(
            arState->swr_ctx,
            "in_sample_rate",
            videoState->audio_ctx->sample_rate,
            0
    );

    // Set SwrContext parameters for resampling
    av_opt_set_sample_fmt(
            arState->swr_ctx,
            "in_sample_fmt",
            videoState->audio_ctx->sample_fmt,
            0
    );

    // Set SwrContext parameters for resampling
    av_opt_set_int(
            arState->swr_ctx,
            "out_channel_layout",
            arState->out_channel_layout,
            0
    );

    // Set SwrContext parameters for resampling
    av_opt_set_int(
            arState->swr_ctx,
            "out_sample_rate",
            videoState->audio_ctx->sample_rate,
            0
    );

    // Set SwrContext parameters for resampling
    av_opt_set_sample_fmt(
            arState->swr_ctx,
            "out_sample_fmt",
            out_sample_fmt,
            0
    );

    // initialize SWR context after user parameters have been set
    int ret = swr_init(arState->swr_ctx);;
    if (ret < 0)
    {
        printf("Failed to initialize the resampling context.\n");
        return -1;
    }

    arState->max_out_nb_samples = arState->out_nb_samples = av_rescale_rnd(
            arState->in_nb_samples,
            videoState->audio_ctx->sample_rate,
            videoState->audio_ctx->sample_rate,
            AV_ROUND_UP
    );

    // check rescaling was successful
    if (arState->max_out_nb_samples <= 0)
    {
        printf("av_rescale_rnd error.\n");
        return -1;
    }

    // get number of output audio channels
    arState->out_nb_channels = av_get_channel_layout_nb_channels(arState->out_channel_layout);

    // allocate data pointers array for arState->resampled_data and fill data
    // pointers and linesize accordingly
    ret = av_samples_alloc_array_and_samples(
            &arState->resampled_data,
            &arState->out_linesize,
            arState->out_nb_channels,
            arState->out_nb_samples,
            out_sample_fmt,
            0
    );

    // check memory allocation for the resampled data was successful
    if (ret < 0)
    {
        printf("av_samples_alloc_array_and_samples() error: Could not allocate destination samples.\n");
        return -1;
    }

    // retrieve output samples number taking into account the progressive delay
    arState->out_nb_samples = av_rescale_rnd(
            swr_get_delay(arState->swr_ctx, videoState->audio_ctx->sample_rate) + arState->in_nb_samples,
            videoState->audio_ctx->sample_rate,
            videoState->audio_ctx->sample_rate,
            AV_ROUND_UP
    );

    // check output samples number was correctly rescaled
    if (arState->out_nb_samples <= 0)
    {
        printf("av_rescale_rnd error\n");
        return -1;
    }

    if (arState->out_nb_samples > arState->max_out_nb_samples)
    {
        // free memory block and set pointer to NULL
        av_free(arState->resampled_data[0]);

        // Allocate a samples buffer for out_nb_samples samples
        ret = av_samples_alloc(
                arState->resampled_data,
                &arState->out_linesize,
                arState->out_nb_channels,
                arState->out_nb_samples,
                out_sample_fmt,
                1
        );

        // check samples buffer correctly allocated
        if (ret < 0)
        {
            printf("av_samples_alloc failed.\n");
            return -1;
        }

        arState->max_out_nb_samples = arState->out_nb_samples;
    }

    if (arState->swr_ctx)
    {
        // do the actual audio data resampling
        ret = swr_convert(
                arState->swr_ctx,
                arState->resampled_data,
                arState->out_nb_samples,
                (const uint8_t **) decoded_audio_frame->data,
                decoded_audio_frame->nb_samples
        );

        // check audio conversion was successful
        if (ret < 0)
        {
            printf("swr_convert_error.\n");
            return -1;
        }

        // get the required buffer size for the given audio parameters
        arState->resampled_data_size = av_samples_get_buffer_size(
                &arState->out_linesize,
                arState->out_nb_channels,
                ret,
                out_sample_fmt,
                1
        );

        // check audio buffer size
        if (arState->resampled_data_size < 0)
        {
            printf("av_samples_get_buffer_size error.\n");
            return -1;
        }
    }
    else
    {
        printf("swr_ctx null error.\n");
        return -1;
    }

    // copy the resampled data to the output buffer
    memcpy(out_buf, arState->resampled_data[0], arState->resampled_data_size);

    /*
     * Memory Cleanup.
     */
    if (arState->resampled_data)
    {
        // free memory block and set pointer to NULL
        av_freep(&arState->resampled_data[0]);
    }

    av_freep(&arState->resampled_data);
    arState->resampled_data = NULL;

    if (arState->swr_ctx)
    {
        // free the allocated SwrContext and set the pointer to NULL
        swr_free(&arState->swr_ctx);
    }

    return arState->resampled_data_size;
}

/**
 * Initializes an instance of the AudioResamplingState Struct with the given
 * parameters.
 *
 * @param   channel_layout  the audio codec context channel layout to be used.
 *
 * @return                  the allocated and initialized AudioResamplingState
 *                          struct instance.
 */
AudioResamplingState * getAudioResampling(uint64_t channel_layout)
{
    AudioResamplingState * audioResampling = av_mallocz(sizeof(AudioResamplingState));

    audioResampling->swr_ctx = swr_alloc();
    audioResampling->in_channel_layout = channel_layout;
    audioResampling->out_channel_layout = AV_CH_LAYOUT_STEREO;
    audioResampling->out_nb_channels = 0;
    audioResampling->out_linesize = 0;
    audioResampling->in_nb_samples = 0;
    audioResampling->out_nb_samples = 0;
    audioResampling->max_out_nb_samples = 0;
    audioResampling->resampled_data = NULL;
    audioResampling->resampled_data_size = 0;

    return audioResampling;
}

/**
 *
 * @param videoState
 * @param pos
 * @param rel
 */
void stream_seek(VideoState * videoState, int64_t pos, int rel)
{
    if (!videoState->seek_req)
    {
        videoState->seek_pos = pos;
        videoState->seek_flags = rel < 0 ? AVSEEK_FLAG_BACKWARD : 0;
        videoState->seek_req = 1;
    }
}