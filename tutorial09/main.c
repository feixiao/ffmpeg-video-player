/*
 * 使用avio解码音频，主要的测试格式aac和mp3
 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF_SIZE 20480

static char *av_get_err(int errnum) {
  static char err_buf[128] = {0};
  av_strerror(errnum, err_buf, 128);
  return err_buf;
}

static void print_sample_format(const AVFrame *frame) {
  printf("ar-samplerate: %uHz\n", frame->sample_rate);
  printf("ac-channel: %u\n", frame->channels);
  printf("f-format: %u\n",
         frame->format);  // 格式需要注意，实际存储到本地文件时已经改成交错模式
}

static int read_packet(void *opaque, uint8_t *buf, int buf_size) {
  FILE *in_file = (FILE *)opaque;
  int read_size = fread(buf, 1, buf_size, in_file);
  printf("read_packet read_size:%d, buf_size:%d\n", read_size, buf_size);
  if (read_size <= 0) {
    return AVERROR_EOF;  // 数据读取完毕
  }
  return read_size;
}

static void decode(AVCodecContext *dec_ctx, AVPacket *packet, AVFrame *frame,
                   FILE *outfile) {
  int ret = 0;
  ret = avcodec_send_packet(dec_ctx, packet);
  if (ret == AVERROR(EAGAIN)) {
    printf(
        "Receive_frame and send_packet both returned EAGAIN, which is an API "
        "violation.\n");
  } else if (ret < 0) {
    printf("Error submitting the packet to the decoder, err:%s\n",
           av_get_err(ret));
    return;
  }

  while (ret >= 0) {
    ret = avcodec_receive_frame(dec_ctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return;
    } else if (ret < 0) {
      printf("Error during decoding\n");
      exit(1);
    }
    if (!packet) {
      printf("get flush frame\n");
    }
    int data_size = av_get_bytes_per_sample(dec_ctx->sample_fmt);
    //        print_sample_format(frame);
    /**
       P表示Planar（平面），其数据格式排列方式为 :
       LLLLLLRRRRRRLLLLLLRRRRRRLLLLLLRRRRRRL...（每个LLLLLLRRRRRR为一个音频帧）
       而不带P的数据格式（即交错排列）排列方式为：
       LRLRLRLRLRLRLRLRLRLRLRLRLRLRLRLRLRLRL...（每个LR为一个音频样本）
    播放范例：   ffplay -ar 48000 -ac 2 -f f32le believe.pcm
        并不是每一种都是这样的格式
    */
    // 这里的写法不是通用，通用要调用重采样的函数去实现
    // 这里只是针对解码出来是planar格式的转换
    for (int i = 0; i < frame->nb_samples; i++) {
      for (int ch = 0; ch < dec_ctx->channels; ch++) {
        fwrite(frame->data[ch] + data_size * i, 1, data_size, outfile);
      }
    }
  }
}

// 运行
// ./tutorial09/tutorial09 ../人世间.aac ../人世间.pcm
// 验证数据
// ffplay -ar 48000 -ac 2 -f f32le 人世间.pcm
int main(int argc, char **argv) {
  if (argc != 3) {
    printf("usage: %s <intput file> <out file>\n", argv[0]);
    return -1;
  }
  const char *in_file_name = argv[1];
  const char *out_file_name = argv[2];
  FILE *in_file = NULL;
  FILE *out_file = NULL;

  // 1.打开参数文件
  in_file = fopen(in_file_name, "rb");
  if (in_file == NULL) {
    printf("open file %s failed\n", in_file_name);
    return -1;
  }

  out_file = fopen(out_file_name, "wb");
  if (out_file == NULL) {
    printf("open file %s failed\n", out_file_name);
    return -1;
  }

  // 2.自定义io
  uint8_t *io_buffer = av_malloc(BUF_SIZE);
  AVIOContext *avio_ctx = avio_alloc_context(
      io_buffer, BUF_SIZE, 0, (void *)in_file, read_packet, NULL, NULL);

  // 3.打开文件，将自定义io做绑定
  AVFormatContext *format_ctx = avformat_alloc_context();
  format_ctx->pb = avio_ctx;
  int ret = avformat_open_input(&format_ctx, NULL, NULL, NULL);
  if (ret < 0) {
    printf("avformat_open_input failed:%s\n", av_err2str(ret));
    return -1;
  }

  // 4.查找指定编码器
  AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
  if (!codec) {
    printf("avcodec_find_decoder failed\n");
    return -1;
  }

  AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
  if (!codec_ctx) {
    printf("avcodec_alloc_context3 failed\n");
    return -1;
  }
  ret = avcodec_open2(codec_ctx, codec, NULL);
  if (ret < 0) {
    printf("avcodec_open2 failed:%s\n", av_err2str(ret));
    return -1;
  }

  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();

  // 5.循环读取packet进行解码，然后存储到输出文件
  while (1) {
    ret = av_read_frame(format_ctx, packet);
    if (ret < 0) {
      printf("av_read_frame failed:%s\n", av_err2str(ret));
      break;
    }
    decode(codec_ctx, packet, frame, out_file);
  }

  printf("read file finish\n");
  decode(codec_ctx, NULL, frame, out_file);

  printf("decode file finish\n");

  fclose(in_file);
  fclose(out_file);

  //   av_free(io_buffer);  // double free

  av_frame_free(frame);
  av_packet_free(packet);

  avformat_close_input(&format_ctx);
  avcodec_free_context(&codec_ctx);

  printf("main finish\n");

  return 0;
}