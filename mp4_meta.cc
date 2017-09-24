/*
  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "mp4_meta.h"

static mp4_atom_handler mp4_atoms[] = {{"ftyp", &Mp4Meta::mp4_read_ftyp_atom},//表明文件类型
                                       {"moov", &Mp4Meta::mp4_read_moov_atom},//包含了媒体metadata信息,包含1个“mvhd”和若干个“trak”,子box
                                       {"mdat", &Mp4Meta::mp4_read_mdat_atom},//存放了媒体数据
                                       {nullptr, nullptr}};

static mp4_atom_handler mp4_moov_atoms[] = {{"mvhd", &Mp4Meta::mp4_read_mvhd_atom},//文件总体信息，如时长，创建时间等
                                            {"trak", &Mp4Meta::mp4_read_trak_atom},//存放视频，音频的容器  包括video trak,audio trak
                                            {"cmov", &Mp4Meta::mp4_read_cmov_atom},//
                                            {nullptr, nullptr}};

static mp4_atom_handler mp4_trak_atoms[] = {{"tkhd", &Mp4Meta::mp4_read_tkhd_atom},//track的总体信息，如时长，高宽等
                                            {"mdia", &Mp4Meta::mp4_read_mdia_atom},//定义了track媒体类型以及sample数据，描述sample信息
                                            {nullptr, nullptr}};

static mp4_atom_handler mp4_mdia_atoms[] = {{"mdhd", &Mp4Meta::mp4_read_mdhd_atom},//定义了timescale,trak需要通过timescale换算成真实时间
                                            {"hdlr", &Mp4Meta::mp4_read_hdlr_atom},//表明trak类型，是video/audio/hint
                                            {"minf", &Mp4Meta::mp4_read_minf_atom},//数据在子box中
                                            {nullptr, nullptr}};

static mp4_atom_handler mp4_minf_atoms[] = {{"vmhd", &Mp4Meta::mp4_read_vmhd_atom},//
                                            {"smhd", &Mp4Meta::mp4_read_smhd_atom},
                                            {"dinf", &Mp4Meta::mp4_read_dinf_atom},
                                            {"stbl", &Mp4Meta::mp4_read_stbl_atom},//sample table box存放是时间/偏移的映射关系表
                                            {nullptr, nullptr}};

static mp4_atom_handler mp4_stbl_atoms[] = {
  {"stsd", &Mp4Meta::mp4_read_stsd_atom},
  {"stts", &Mp4Meta::mp4_read_stts_atom},// time to sample, 时间戳—sample序号 映射表
  {"stss", &Mp4Meta::mp4_read_stss_atom},//确定media中的关键帧
  {"ctts", &Mp4Meta::mp4_read_ctts_atom},
  {"stsc", &Mp4Meta::mp4_read_stsc_atom},// sample to chunk, sample 和chunk 映射表
  {"stsz", &Mp4Meta::mp4_read_stsz_atom},// sample size , 每个sample 大小
  {"stco", &Mp4Meta::mp4_read_stco_atom},//chunk offset, 每个chunk的偏移，sample的偏移可根据其他box推算出来
  {"co64", &Mp4Meta::mp4_read_co64_atom},//64-bit chunk offseet
  {nullptr, nullptr}};

static void mp4_reader_set_32value(TSIOBufferReader readerp, int64_t offset, uint32_t n);
static void mp4_reader_set_64value(TSIOBufferReader readerp, int64_t offset, uint64_t n);
static uint32_t mp4_reader_get_32value(TSIOBufferReader readerp, int64_t offset);
static uint64_t mp4_reader_get_64value(TSIOBufferReader readerp, int64_t offset);
static int64_t IOBufferReaderCopy(TSIOBufferReader readerp, void *buf, int64_t length);

int
Mp4Meta::parse_meta(bool body_complete)
{
  int ret, rc;

  meta_avail = TSIOBufferReaderAvail(meta_reader);

  if (wait_next && wait_next <= meta_avail) {
    mp4_meta_consume(wait_next);
    wait_next = 0;
  }

  if (meta_avail < MP4_MIN_BUFFER_SIZE && !body_complete) {
    return 0;
  }

  //先解析mp4 头
  ret = this->parse_root_atoms();

  if (ret < 0) {
    return -1;

  } else if (ret == 0) {
    if (body_complete) {
      return -1;

    } else {
      return 0;
    }
  }

  // generate new meta data
  //然后进行 start end 操作
  rc = this->post_process_meta();
  TSDebug(PLUGIN_NAME, "end post_process_meta rc = %d", rc);
  if (rc != 0) {
    return -1;
  }

  return 1;
}

void
Mp4Meta::mp4_meta_consume(int64_t size)
{
  TSIOBufferReaderConsume(meta_reader, size);
  meta_avail -= size;
  passed += size;
}

int//开始进行moov box 修改
Mp4Meta::post_process_meta()
{
  //偏移 ， 调整
  off_t start_offset, adjustment, end_offset;
  uint32_t i, j;
  int64_t avail;
  Mp4Trak *trak;

  if (this->trak_num == 0) {
    return -1;
  }

  if (mdat_atom.buffer == nullptr) {// 如果先读到mdat 用来存储媒体数据，就按照失败处理
    return -1;
  }

  out_handle.buffer = TSIOBufferCreate();
  out_handle.reader = TSIOBufferReaderAlloc(out_handle.buffer);

  if (ftyp_atom.buffer) {// 不用修改直接copy
    TSIOBufferCopy(out_handle.buffer, ftyp_atom.reader, TSIOBufferReaderAvail(ftyp_atom.reader), 0);
  }

  if (moov_atom.buffer) {// moov header
    TSIOBufferCopy(out_handle.buffer, moov_atom.reader, TSIOBufferReaderAvail(moov_atom.reader), 0);
  }

  if (mvhd_atom.buffer) {// mvhd
    avail = TSIOBufferReaderAvail(mvhd_atom.reader);
    TSIOBufferCopy(out_handle.buffer, mvhd_atom.reader, avail, 0);
    this->moov_size += avail; //计算move size
  }

  start_offset = cl;
  //start_offset= 86812929
  end_offset = 0;
  TSDebug(PLUGIN_NAME, "[post_process_meta] start_offset= %ld", start_offset);
  for (i = 0; i < trak_num; i++) {
    trak = trak_vec[i];
    TSDebug(PLUGIN_NAME, "[post_process_meta] for trak_num[i]= %lu", i);
    if (mp4_update_stts_atom(trak) != 0) {
      return -1;
    }
      TSDebug(PLUGIN_NAME, "[post_process_meta] 1 trak->size= %ld", trak->size);
    if (mp4_update_stss_atom(trak) != 0) {
      return -1;
    }
      TSDebug(PLUGIN_NAME, "[post_process_meta] 2 trak->size= %ld", trak->size);
    mp4_update_ctts_atom(trak);
      TSDebug(PLUGIN_NAME, "[post_process_meta] 3 trak->size= %ld", trak->size);
    if (mp4_update_stsc_atom(trak) != 0) {
      return -1;
    }
      TSDebug(PLUGIN_NAME, "[post_process_meta] 4 trak->size= %ld", trak->size);
    if (mp4_update_stsz_atom(trak) != 0) {
      return -1;
    }
      TSDebug(PLUGIN_NAME, "[post_process_meta] 5 trak->size= %ld", trak->size);
    if (trak->atoms[MP4_CO64_DATA].buffer) {
      if (mp4_update_co64_atom(trak) != 0) {
        return -1;
      }

    } else if (mp4_update_stco_atom(trak) != 0) {
      return -1;
    }
      TSDebug(PLUGIN_NAME, "[post_process_meta] 6 trak->size= %ld", trak->size);
    mp4_update_stbl_atom(trak);
      TSDebug(PLUGIN_NAME, "[post_process_meta] 7 trak->size= %ld", trak->size);
    mp4_update_minf_atom(trak);
      TSDebug(PLUGIN_NAME, "[post_process_meta] 8 trak->size= %ld", trak->size);
    trak->size += trak->mdhd_size;
    trak->size += trak->hdlr_size;
    mp4_update_mdia_atom(trak);
      TSDebug(PLUGIN_NAME, "[post_process_meta] 9 trak->size= %ld", trak->size);
    trak->size += trak->tkhd_size;
    mp4_update_trak_atom(trak);
      TSDebug(PLUGIN_NAME, "[post_process_meta] 10 trak->size= %ld", trak->size);

    this->moov_size += trak->size;//moov size = mvhd size + trak size
    TSDebug(PLUGIN_NAME, "[post_process_meta] for moov_size= %ld, trak->size= %ld", this->moov_size, trak->size);
    //trak->start_offset 每个trak 的偏移量
    //因为包含了多个trak 列入 video trak, audio trak 所以多者之间要找最小的start_offset
    if (start_offset > trak->start_offset) {
      start_offset = trak->start_offset;
    }

      if ((this->end > 0) && end_offset < trak->end_offset) {
          end_offset = trak->end_offset;
      }
      TSDebug(PLUGIN_NAME, "[post_process_meta] start_offset = %ld, end_offset=%ld", start_offset,end_offset);

    for (j = 0; j <= MP4_LAST_ATOM; j++) {
      if (trak->atoms[j].buffer) {
        TSIOBufferCopy(out_handle.buffer, trak->atoms[j].reader, TSIOBufferReaderAvail(trak->atoms[j].reader), 0);
      }
    }

    mp4_update_tkhd_duration(trak);//更新duration
    mp4_update_mdhd_duration(trak);//更新duration
  }



  this->moov_size += 8;//加上本身的 size + name 大小

  mp4_reader_set_32value(moov_atom.reader, 0, this->moov_size);
  this->content_length += this->moov_size;// content_length = ftype+ moov size
  TSDebug(PLUGIN_NAME, "[post_process_meta] content_length= %ld, moov_size=%ld", this->content_length,this->moov_size);
  //this->content_length + (cl-start_offset)的长度 + mdat header size
  //为一个负数，丢弃了多少字节
  adjustment = this->ftyp_size + this->moov_size + mp4_update_mdat_atom(start_offset, end_offset) - start_offset;
  TSDebug(PLUGIN_NAME, "[post_process_meta] adjustment=%ld,ftyp=%ld, moov_size=%ld, start_offset= %ld, mdat_header=%ld",
          adjustment,this->ftyp_size,this->moov_size,start_offset,(start_offset+adjustment -this->ftyp_size -this->moov_size));
  TSIOBufferCopy(out_handle.buffer, mdat_atom.reader, TSIOBufferReaderAvail(mdat_atom.reader), 0);

  for (i = 0; i < trak_num; i++) {
    trak = trak_vec[i];

    //减去偏移量
    if (trak->atoms[MP4_CO64_DATA].buffer) {
      mp4_adjust_co64_atom(trak, adjustment);

    } else {
      mp4_adjust_stco_atom(trak, adjustment);
    }
  }

  mp4_update_mvhd_duration();//更新duration
  TSDebug(PLUGIN_NAME, "[post_process_meta] last  content_length= %ld", this->content_length);
  return 0;
}

/*
 * -1: error
 *  0: unfinished
 *  1: success.
 */
int
Mp4Meta::parse_root_atoms()
{
  int i, ret, rc;
  int64_t atom_size, atom_header_size, copied_size;
  char buf[64];
  char *atom_header, *atom_name;

  memset(buf, 0, sizeof(buf));

  for (;;) {
    if (meta_avail < (int64_t)sizeof(uint32_t)) {
      return 0;
    }

    //size指明了整个box所占用的大小，包括header部分。如果box很大(例如存放具体视频数据的mdat box)，
    // 超过了uint32的最大数值，size就被设置为1，并用接下来的8位uint64来存放大小。
    copied_size = IOBufferReaderCopy(meta_reader, buf, sizeof(mp4_atom_header64));
    atom_size   = copied_size > 0 ? mp4_get_32value(buf) : 0;

    if (atom_size == 0) {//如果size 大小为0 说明是最后一个box 直接结束
      return 1;
    }

    atom_header = buf;

    if (atom_size < (int64_t)sizeof(mp4_atom_header)) {//判断是否满足一个32位头大小，如果小于的话，一定是64位的
      if (atom_size == 1) {//如果size 为1 说明是64位的
        if (meta_avail < (int64_t)sizeof(mp4_atom_header64)) { //如果总数据小于64 box header 大小，就再次等待
          return 0;
        }

      } else {//如果不满足，说明解释失败，直接返回error
        return -1;
      }

      atom_size        = mp4_get_64value(atom_header + 8);
      atom_header_size = sizeof(mp4_atom_header64);

    } else { // regular atom

      if (meta_avail < (int64_t)sizeof(mp4_atom_header)) { // not enough for atom header  再次等待数据过来
        return 0;
      }

      atom_header_size = sizeof(mp4_atom_header);
    }

    atom_name = atom_header + 4;

    if (atom_size + this->passed > this->cl) {//超过总长度
      return -1;
    }

    for (i = 0; mp4_atoms[i].name; i++) {// box header + box body
      if (memcmp(atom_name, mp4_atoms[i].name, 4) == 0) {
        ret = (this->*mp4_atoms[i].handler)(atom_header_size, atom_size - atom_header_size); // -1: error, 0: unfinished, 1: success

        if (ret <= 0) {
          return ret;

        } else if (meta_complete) { // success
          return 1;
        }

        goto next;
      }
    }

    // nonsignificant atom box
    rc = mp4_atom_next(atom_size, true); // 0: unfinished, 1: success
    if (rc == 0) {
      return rc;
    }

  next:
    continue;
  }

  return 1;
}

int
Mp4Meta::mp4_atom_next(int64_t atom_size, bool wait)
{
  if (meta_avail >= atom_size) {
    mp4_meta_consume(atom_size);
    return 1;
  }

  if (wait) {
    wait_next = atom_size;
    return 0;
  }

  return -1;
}

/*
 *  -1: error
 *   1: success
 */
int
Mp4Meta::mp4_read_atom(mp4_atom_handler *atom, int64_t size)
{
  int i, ret, rc;
  int64_t atom_size, atom_header_size, copied_size;
  char buf[32];
  char *atom_header, *atom_name;

  if (meta_avail < size) { // data insufficient, not reasonable for internal atom box. 数据应该是全的
    return -1;
  }

  while (size > 0) {
    if (meta_avail < (int64_t)sizeof(uint32_t)) { // data insufficient, not reasonable for internal atom box.
      return -1;
    }

    copied_size = IOBufferReaderCopy(meta_reader, buf, sizeof(mp4_atom_header64));
    atom_size   = copied_size > 0 ? mp4_get_32value(buf) : 0;

    if (atom_size == 0) {
      return 1;
    }

    atom_header = buf;

    if (atom_size < (int64_t)sizeof(mp4_atom_header)) { //判断是32位还是64位的
      if (atom_size == 1) {
        if (meta_avail < (int64_t)sizeof(mp4_atom_header64)) {
          return -1;
        }

      } else {
        return -1;
      }

      atom_size        = mp4_get_64value(atom_header + 8);
      atom_header_size = sizeof(mp4_atom_header64);

    } else { // regular atom

      if (meta_avail < (int64_t)sizeof(mp4_atom_header)) {
        return -1;
      }

      atom_header_size = sizeof(mp4_atom_header);
    }

    atom_name = atom_header + 4;

    if (atom_size + this->passed > this->cl) { //判断一下总长度
      return -1;
    }

    for (i = 0; atom[i].name; i++) {
      if (memcmp(atom_name, atom[i].name, 4) == 0) {
        if (meta_avail < atom_size) {
          return -1;
        }

        ret = (this->*atom[i].handler)(atom_header_size, atom_size - atom_header_size); // -1: error, 0: success.

        if (ret < 0) {
          return ret;
        }

        goto next;
      }
    }

    // insignificant atom box
    rc = mp4_atom_next(atom_size, false); //可以忽视的box
    if (rc < 0) {
      return rc;
    }

  next:
    size -= atom_size;
    continue;
  }

  return 1;
}

int//拷贝ftyp数据，不需要做任何处理
Mp4Meta::mp4_read_ftyp_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int64_t atom_size;

  if (atom_data_size > MP4_MIN_BUFFER_SIZE) {
    return -1;
  }

  atom_size = atom_header_size + atom_data_size;

  if (meta_avail < atom_size) { // data unsufficient, reasonable from the first level
    return 0;
  }

  ftyp_atom.buffer = TSIOBufferCreate();
  ftyp_atom.reader = TSIOBufferReaderAlloc(ftyp_atom.buffer);

  TSIOBufferCopy(ftyp_atom.buffer, meta_reader, atom_size, 0);
  mp4_meta_consume(atom_size);

  content_length = atom_size;//文件长度
  ftyp_size      = atom_size;

  return 1;
}

int//读取moov
Mp4Meta::mp4_read_moov_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int64_t atom_size;
  int ret;

  if (mdat_atom.buffer != nullptr) { // not reasonable for streaming media 如果先读的mdata 的话，就当失败来处理
    return -1;
  }

  atom_size = atom_header_size + atom_data_size;

  if (atom_data_size >= MP4_MAX_BUFFER_SIZE) { //如果大于限定的buffer 当出错处理
    return -1;
  }

  if (meta_avail < atom_size) { // data unsufficient, wait 数据不全，继续等待
    return 0;
  }

  moov_atom.buffer = TSIOBufferCreate();
  moov_atom.reader = TSIOBufferReaderAlloc(moov_atom.buffer);

  TSIOBufferCopy(moov_atom.buffer, meta_reader, atom_header_size, 0); //先拷贝 BOX HEADER
  mp4_meta_consume(atom_header_size);

  ret = mp4_read_atom(mp4_moov_atoms, atom_data_size);//开始解析mvhd + track.........

  return ret;
}

int
Mp4Meta::mp4_read_mvhd_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int64_t atom_size;
  uint32_t timescale;
  mp4_mvhd_atom *mvhd;
  mp4_mvhd64_atom mvhd64;

  if (sizeof(mp4_mvhd_atom) - 8 > (size_t)atom_data_size) {
    return -1;
  }

  memset(&mvhd64, 0, sizeof(mvhd64));
  IOBufferReaderCopy(meta_reader, &mvhd64, sizeof(mp4_mvhd64_atom));
  mvhd = (mp4_mvhd_atom *)&mvhd64;

  if (mvhd->version[0] == 0) {
    timescale = mp4_get_32value(mvhd->timescale);

  } else { // 64-bit duration
    timescale = mp4_get_32value(mvhd64.timescale);
  }

  this->timescale = timescale;  //获取整部电影的time scale

  atom_size = atom_header_size + atom_data_size;

  mvhd_atom.buffer = TSIOBufferCreate();
  mvhd_atom.reader = TSIOBufferReaderAlloc(mvhd_atom.buffer);

  TSIOBufferCopy(mvhd_atom.buffer, meta_reader, atom_size, 0);
  mp4_meta_consume(atom_size);

  return 1;
}

int//读取track
Mp4Meta::mp4_read_trak_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int rc;
  Mp4Trak *trak;

  if (trak_num >= MP4_MAX_TRAK_NUM - 1) {
    return -1;
  }

  trak                 = new Mp4Trak();
  trak_vec[trak_num++] = trak;
  TSDebug(PLUGIN_NAME, "[mp4_read_trak_atom] trak_num = %lu", trak_num-1);

  trak->atoms[MP4_TRAK_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_TRAK_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_TRAK_ATOM].buffer);

  TSIOBufferCopy(trak->atoms[MP4_TRAK_ATOM].buffer, meta_reader, atom_header_size, 0);// box header
  mp4_meta_consume(atom_header_size);

  rc = mp4_read_atom(mp4_trak_atoms, atom_data_size);//读取tkhd + media

  return rc;
}

int Mp4Meta::mp4_read_cmov_atom(int64_t /*atom_header_size ATS_UNUSED */, int64_t /* atom_data_size ATS_UNUSED */)
{
  return -1;
}

int
Mp4Meta::mp4_read_tkhd_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int64_t atom_size;
  Mp4Trak *trak;

  atom_size = atom_header_size + atom_data_size;

  trak            = trak_vec[trak_num - 1];
  trak->tkhd_size = atom_size;//track header box size

  trak->atoms[MP4_TKHD_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_TKHD_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_TKHD_ATOM].buffer);

  TSIOBufferCopy(trak->atoms[MP4_TKHD_ATOM].buffer, meta_reader, atom_size, 0);
  mp4_meta_consume(atom_size);

  mp4_reader_set_32value(trak->atoms[MP4_TKHD_ATOM].reader, offsetof(mp4_tkhd_atom, size), atom_size);//设置一下tkhd 的总大小

  return 1;
}

int
Mp4Meta::mp4_read_mdia_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  Mp4Trak *trak;

  trak = trak_vec[trak_num - 1];

  trak->atoms[MP4_MDIA_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_MDIA_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_MDIA_ATOM].buffer);

  TSIOBufferCopy(trak->atoms[MP4_MDIA_ATOM].buffer, meta_reader, atom_header_size, 0);//读取 box header
  mp4_meta_consume(atom_header_size);

  return mp4_read_atom(mp4_mdia_atoms, atom_data_size);
}

int
Mp4Meta::mp4_read_mdhd_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int64_t atom_size, duration;
  uint32_t ts;
  Mp4Trak *trak;
  mp4_mdhd_atom *mdhd;
  mp4_mdhd64_atom mdhd64;

  memset(&mdhd64, 0, sizeof(mdhd64));
  IOBufferReaderCopy(meta_reader, &mdhd64, sizeof(mp4_mdhd64_atom));
  mdhd = (mp4_mdhd_atom *)&mdhd64;

  if (mdhd->version[0] == 0) {
    ts       = mp4_get_32value(mdhd->timescale);
    duration = mp4_get_32value(mdhd->duration);

  } else {
    ts       = mp4_get_32value(mdhd64.timescale);
    duration = mp4_get_64value(mdhd64.duration);
  }

  atom_size = atom_header_size + atom_data_size;

  trak            = trak_vec[trak_num - 1];
  trak->mdhd_size = atom_size;
  //时长 = duration / timescale
  trak->timescale = ts;
  trak->duration  = duration;

  trak->atoms[MP4_MDHD_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_MDHD_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_MDHD_ATOM].buffer);

  TSIOBufferCopy(trak->atoms[MP4_MDHD_ATOM].buffer, meta_reader, atom_size, 0);
  mp4_meta_consume(atom_size);

  mp4_reader_set_32value(trak->atoms[MP4_MDHD_ATOM].reader, offsetof(mp4_mdhd_atom, size), atom_size);//重新设置大小

  return 1;
}

int
Mp4Meta::mp4_read_hdlr_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int64_t atom_size;
  Mp4Trak *trak;

  atom_size = atom_header_size + atom_data_size;

  trak            = trak_vec[trak_num - 1];
  trak->hdlr_size = atom_size;

  trak->atoms[MP4_HDLR_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_HDLR_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_HDLR_ATOM].buffer);

  TSIOBufferCopy(trak->atoms[MP4_HDLR_ATOM].buffer, meta_reader, atom_size, 0);
  mp4_meta_consume(atom_size);

  return 1;
}

int
Mp4Meta::mp4_read_minf_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  Mp4Trak *trak;

  trak = trak_vec[trak_num - 1];

  trak->atoms[MP4_MINF_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_MINF_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_MINF_ATOM].buffer);

  TSIOBufferCopy(trak->atoms[MP4_MINF_ATOM].buffer, meta_reader, atom_header_size, 0);
  mp4_meta_consume(atom_header_size);

  return mp4_read_atom(mp4_minf_atoms, atom_data_size);
}

int
Mp4Meta::mp4_read_vmhd_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int64_t atom_size;
  Mp4Trak *trak;

  atom_size = atom_data_size + atom_header_size;

  trak = trak_vec[trak_num - 1];
  trak->vmhd_size += atom_size;

  trak->atoms[MP4_VMHD_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_VMHD_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_VMHD_ATOM].buffer);

  TSIOBufferCopy(trak->atoms[MP4_VMHD_ATOM].buffer, meta_reader, atom_size, 0);
  mp4_meta_consume(atom_size);

  return 1;
}

int
Mp4Meta::mp4_read_smhd_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int64_t atom_size;
  Mp4Trak *trak;

  atom_size = atom_data_size + atom_header_size;

  trak = trak_vec[trak_num - 1];
  trak->smhd_size += atom_size;

  trak->atoms[MP4_SMHD_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_SMHD_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_SMHD_ATOM].buffer);

  TSIOBufferCopy(trak->atoms[MP4_SMHD_ATOM].buffer, meta_reader, atom_size, 0);
  mp4_meta_consume(atom_size);

  return 1;
}

int
Mp4Meta::mp4_read_dinf_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int64_t atom_size;
  Mp4Trak *trak;

  atom_size = atom_data_size + atom_header_size;

  trak = trak_vec[trak_num - 1];
  trak->dinf_size += atom_size;

  trak->atoms[MP4_DINF_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_DINF_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_DINF_ATOM].buffer);

  TSIOBufferCopy(trak->atoms[MP4_DINF_ATOM].buffer, meta_reader, atom_size, 0);
  mp4_meta_consume(atom_size);

  return 1;
}

int
Mp4Meta::mp4_read_stbl_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  Mp4Trak *trak;

  trak = trak_vec[trak_num - 1];

  trak->atoms[MP4_STBL_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_STBL_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STBL_ATOM].buffer);

  TSIOBufferCopy(trak->atoms[MP4_STBL_ATOM].buffer, meta_reader, atom_header_size, 0);
  mp4_meta_consume(atom_header_size);

  return mp4_read_atom(mp4_stbl_atoms, atom_data_size);
}

int//sample description box
Mp4Meta::mp4_read_stsd_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int64_t atom_size;
  Mp4Trak *trak;

  atom_size = atom_data_size + atom_header_size;

  trak = trak_vec[trak_num - 1];
  trak->size += atom_size;

  trak->atoms[MP4_STSD_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_STSD_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STSD_ATOM].buffer);

  TSIOBufferCopy(trak->atoms[MP4_STSD_ATOM].buffer, meta_reader, atom_size, 0);

  mp4_meta_consume(atom_size);

  return 1;
}

/**
 * time to sample box
 * size, type, version flags, number of entries
 * entry 1: sample count 1, sample duration 42
 * .......
 * 实际时间 0.2s  对应的duration = mdhd.timescale * 0.2s
 */
int
Mp4Meta::mp4_read_stts_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int32_t entries;
  int64_t esize, copied_size;
  mp4_stts_atom stts;
  Mp4Trak *trak;


  if (sizeof(mp4_stts_atom) - 8 > (size_t)atom_data_size) {
    return -1;
  }

  copied_size = IOBufferReaderCopy(meta_reader, &stts, sizeof(mp4_stts_atom));
  entries     = copied_size > 0 ? mp4_get_32value(stts.entries) : 0;
  esize       = entries * sizeof(mp4_stts_entry);

  if (sizeof(mp4_stts_atom) - 8 + esize > (size_t)atom_data_size) {
    return -1;
  }

  trak                         = trak_vec[trak_num - 1];
  trak->time_to_sample_entries = entries;

  trak->atoms[MP4_STTS_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_STTS_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STTS_ATOM].buffer);
  TSIOBufferCopy(trak->atoms[MP4_STTS_ATOM].buffer, meta_reader, sizeof(mp4_stts_atom), 0);

  trak->atoms[MP4_STTS_DATA].buffer = TSIOBufferCreate();
  trak->atoms[MP4_STTS_DATA].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STTS_DATA].buffer);
  TSIOBufferCopy(trak->atoms[MP4_STTS_DATA].buffer, meta_reader, esize, sizeof(mp4_stts_atom));

  mp4_meta_consume(atom_data_size + atom_header_size);

  return 1;
}

/**
 * Sync Sample Box
 * size, type, version flags, number of entries
 *
 * 该box 决定了整个mp4 文件是否可以拖动，如果box 只有一个entry,则拖拉时，进度到最后
 */
int
Mp4Meta::mp4_read_stss_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int32_t entries;
  int64_t esize, copied_size;
  mp4_stss_atom stss;
  Mp4Trak *trak;

  if (sizeof(mp4_stss_atom) - 8 > (size_t)atom_data_size) {
    return -1;
  }

  copied_size = IOBufferReaderCopy(meta_reader, &stss, sizeof(mp4_stss_atom));
  entries     = copied_size > 0 ? mp4_get_32value(stss.entries) : 0;
  esize       = entries * sizeof(int32_t); //sample id

  if (sizeof(mp4_stss_atom) - 8 + esize > (size_t)atom_data_size) {
    return -1;
  }

  trak                       = trak_vec[trak_num - 1];
  trak->sync_samples_entries = entries;

  trak->atoms[MP4_STSS_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_STSS_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STSS_ATOM].buffer);
  TSIOBufferCopy(trak->atoms[MP4_STSS_ATOM].buffer, meta_reader, sizeof(mp4_stss_atom), 0);

  trak->atoms[MP4_STSS_DATA].buffer = TSIOBufferCreate();
  trak->atoms[MP4_STSS_DATA].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STSS_DATA].buffer);
  TSIOBufferCopy(trak->atoms[MP4_STSS_DATA].buffer, meta_reader, esize, sizeof(mp4_stss_atom));

  mp4_meta_consume(atom_data_size + atom_header_size);

  return 1;
}

int//composition time to sample box
Mp4Meta::mp4_read_ctts_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int32_t entries;
  int64_t esize, copied_size;
  mp4_ctts_atom ctts;
  Mp4Trak *trak;

  if (sizeof(mp4_ctts_atom) - 8 > (size_t)atom_data_size) {
    return -1;
  }

  copied_size = IOBufferReaderCopy(meta_reader, &ctts, sizeof(mp4_ctts_atom));
  entries     = copied_size > 0 ? mp4_get_32value(ctts.entries) : 0;
  esize       = entries * sizeof(mp4_ctts_entry);

  if (sizeof(mp4_ctts_atom) - 8 + esize > (size_t)atom_data_size) {
    return -1;
  }

  trak                             = trak_vec[trak_num - 1];
  trak->composition_offset_entries = entries;

  trak->atoms[MP4_CTTS_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_CTTS_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_CTTS_ATOM].buffer);
  TSIOBufferCopy(trak->atoms[MP4_CTTS_ATOM].buffer, meta_reader, sizeof(mp4_ctts_atom), 0);

  trak->atoms[MP4_CTTS_DATA].buffer = TSIOBufferCreate();
  trak->atoms[MP4_CTTS_DATA].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_CTTS_DATA].buffer);
  TSIOBufferCopy(trak->atoms[MP4_CTTS_DATA].buffer, meta_reader, esize, sizeof(mp4_ctts_atom));

  mp4_meta_consume(atom_data_size + atom_header_size);

  return 1;
}

/**
 * sample to chunk box
 * size, type, version, flags, number of entries
 * entry 1:first chunk 1, samples pre chunk 13, sample description 1 ('self-ref')
 * ...............
 * 第500个sample 500 ＝ 28 ＊ 13 ＋ 12 ＋ 13*9 ＋ 7
 *
 * first_chunk 表示 这一组相同类型的chunk中 的第一个chunk数。
 * 这些chunk 中包含的Sample 数量，即samples_per_chunk 是一致的。
 * 每个Sample 可以通过sample_description_index 去stsd box 找到描述信息。
 */
int
Mp4Meta::mp4_read_stsc_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int32_t entries;
  int64_t esize, copied_size;
  mp4_stsc_atom stsc;
  Mp4Trak *trak;

  if (sizeof(mp4_stsc_atom) - 8 > (size_t)atom_data_size) {
    return -1;
  }

  copied_size = IOBufferReaderCopy(meta_reader, &stsc, sizeof(mp4_stsc_atom));
  entries     = copied_size > 0 ? mp4_get_32value(stsc.entries) : 0;
  esize       = entries * sizeof(mp4_stsc_entry);

  if (sizeof(mp4_stsc_atom) - 8 + esize > (size_t)atom_data_size) {
    return -1;
  }

  trak                          = trak_vec[trak_num - 1];
  trak->sample_to_chunk_entries = entries;

  trak->atoms[MP4_STSC_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_STSC_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STSC_ATOM].buffer);
  TSIOBufferCopy(trak->atoms[MP4_STSC_ATOM].buffer, meta_reader, sizeof(mp4_stsc_atom), 0);

  trak->atoms[MP4_STSC_DATA].buffer = TSIOBufferCreate();
  trak->atoms[MP4_STSC_DATA].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STSC_DATA].buffer);
  TSIOBufferCopy(trak->atoms[MP4_STSC_DATA].buffer, meta_reader, esize, sizeof(mp4_stsc_atom));

  mp4_meta_consume(atom_data_size + atom_header_size);

  return 1;
}

/**
 * sample size box
 * size, type, version, flags, sample size, number of entries
 * sample 1: sample size $000000ae
 * ........
 */
int
Mp4Meta::mp4_read_stsz_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int32_t entries, size;
  int64_t esize, atom_size, copied_size;
  mp4_stsz_atom stsz;
  Mp4Trak *trak;

  if (sizeof(mp4_stsz_atom) - 8 > (size_t)atom_data_size) {
    return -1;
  }

  copied_size = IOBufferReaderCopy(meta_reader, &stsz, sizeof(mp4_stsz_atom));
  entries     = copied_size > 0 ? mp4_get_32value(stsz.entries) : 0;
  esize       = entries * sizeof(int32_t); //sample size

  trak = trak_vec[trak_num - 1];
  size = copied_size > 0 ? mp4_get_32value(stsz.uniform_size) : 0;

  trak->sample_sizes_entries = entries;

  trak->atoms[MP4_STSZ_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_STSZ_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STSZ_ATOM].buffer);
  TSIOBufferCopy(trak->atoms[MP4_STSZ_ATOM].buffer, meta_reader, sizeof(mp4_stsz_atom), 0);

  if (size == 0) {//全部sample 数目，如果所有的sample有相同的长度，这个字段就是这个值，否则就是0
    if (sizeof(mp4_stsz_atom) - 8 + esize > (size_t)atom_data_size) {
      return -1;
    }

    trak->atoms[MP4_STSZ_DATA].buffer = TSIOBufferCreate();
    trak->atoms[MP4_STSZ_DATA].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STSZ_DATA].buffer);
    TSIOBufferCopy(trak->atoms[MP4_STSZ_DATA].buffer, meta_reader, esize, sizeof(mp4_stsz_atom));

  } else {
    atom_size = atom_header_size + atom_data_size;
    trak->size += atom_size;
    mp4_reader_set_32value(trak->atoms[MP4_STSZ_ATOM].reader, 0, atom_size);
  }

  mp4_meta_consume(atom_data_size + atom_header_size);

  return 1;
}

/**
 * 32 位 chunk offset box 定义了每个chunk 在媒体流中的位置。
 * size, type, version, flags, number of entries
 * chunk 1: $00039D28(in this file)
 * .........
 */
int
Mp4Meta::mp4_read_stco_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int32_t entries;
  int64_t esize, copied_size;
  mp4_stco_atom stco;
  Mp4Trak *trak;

  if (sizeof(mp4_stco_atom) - 8 > (size_t)atom_data_size) {
    return -1;
  }

  copied_size = IOBufferReaderCopy(meta_reader, &stco, sizeof(mp4_stco_atom));
  entries     = copied_size > 0 ? mp4_get_32value(stco.entries) : 0;
  esize       = entries * sizeof(int32_t);

  if (sizeof(mp4_stco_atom) - 8 + esize > (size_t)atom_data_size) {
    return -1;
  }

  trak         = trak_vec[trak_num - 1];
  trak->chunks = entries;
  // entries = 16391,trak_num=0
  TSDebug(PLUGIN_NAME, "[mp4_read_stco_atom] entries = %d,trak_num=%lu", entries,trak_num-1);
  trak->atoms[MP4_STCO_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_STCO_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STCO_ATOM].buffer);
  TSIOBufferCopy(trak->atoms[MP4_STCO_ATOM].buffer, meta_reader, sizeof(mp4_stco_atom), 0);

  trak->atoms[MP4_STCO_DATA].buffer = TSIOBufferCreate();
  trak->atoms[MP4_STCO_DATA].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STCO_DATA].buffer);
  TSIOBufferCopy(trak->atoms[MP4_STCO_DATA].buffer, meta_reader, esize, sizeof(mp4_stco_atom));

  mp4_meta_consume(atom_data_size + atom_header_size);

  return 1;
}

/**
 *  64 位
 */
int
Mp4Meta::mp4_read_co64_atom(int64_t atom_header_size, int64_t atom_data_size)
{
  int32_t entries;
  int64_t esize, copied_size;
  mp4_co64_atom co64;
  Mp4Trak *trak;

  if (sizeof(mp4_co64_atom) - 8 > (size_t)atom_data_size) {
    return -1;
  }

  copied_size = IOBufferReaderCopy(meta_reader, &co64, sizeof(mp4_co64_atom));
  entries     = copied_size > 0 ? mp4_get_32value(co64.entries) : 0;
  esize       = entries * sizeof(int64_t);

  if (sizeof(mp4_co64_atom) - 8 + esize > (size_t)atom_data_size) {
    return -1;
  }

  trak         = trak_vec[trak_num - 1];
  trak->chunks = entries;
  TSDebug(PLUGIN_NAME, "[mp4_read_co64_atom] entries = %d,trak_num=%lu", entries,trak_num-1);
  trak->atoms[MP4_CO64_ATOM].buffer = TSIOBufferCreate();
  trak->atoms[MP4_CO64_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_CO64_ATOM].buffer);
  TSIOBufferCopy(trak->atoms[MP4_CO64_ATOM].buffer, meta_reader, sizeof(mp4_co64_atom), 0);

  trak->atoms[MP4_CO64_DATA].buffer = TSIOBufferCreate();
  trak->atoms[MP4_CO64_DATA].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_CO64_DATA].buffer);
  TSIOBufferCopy(trak->atoms[MP4_CO64_DATA].buffer, meta_reader, esize, sizeof(mp4_co64_atom));

  mp4_meta_consume(atom_data_size + atom_header_size);

  return 1;
}

/**
 * 当读到mdat 的时候说明解析成功了
 */
int Mp4Meta::mp4_read_mdat_atom(int64_t /* atom_header_size ATS_UNUSED */, int64_t /* atom_data_size ATS_UNUSED */)
{
  mdat_atom.buffer = TSIOBufferCreate();
  mdat_atom.reader = TSIOBufferReaderAlloc(mdat_atom.buffer);

  meta_complete = true;
  return 1;
}

int Mp4Meta::mp4_crop_stts_data_start(Mp4Trak *trak)
{
    uint32_t i, entries, count, duration, pass;
    uint32_t start_sample, left;
    uint32_t key_sample, old_sample;
    uint64_t start_time, sum;
    int64_t atom_size;
    TSIOBufferReader readerp;

    TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_start] --------------------------start-------------------------");
    if (trak->atoms[MP4_STTS_DATA].buffer == nullptr) {
        return -1;
    }

    if (this->end > 0) {
        this->mp4_crop_stts_data_end(trak);
    }

    sum = 0;

    entries    = trak->time_to_sample_entries;
    //time = duration / timescale      duration = time * timescale / 1000 (将start 毫秒转为秒)
    start_time = this->start * trak->timescale / 1000;
    if (this->rs > 0) {
        start_time = (uint64_t)(this->rs * trak->timescale / 1000);
    }

    TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_start] rs= %lf",this->rs);
    //mp4_update_stts_atom start_time = 48000000 entries=2
    TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_start] start_time = %ld entries=%d",start_time, entries);
    start_sample = 0;//开始start_sample
    readerp      = TSIOBufferReaderClone(trak->atoms[MP4_STTS_DATA].reader);

    for (i = 0; i < entries; i++) {//根据时间查找
        duration = (uint32_t)mp4_reader_get_32value(readerp, offsetof(mp4_stts_entry, duration));
        count    = (uint32_t)mp4_reader_get_32value(readerp, offsetof(mp4_stts_entry, count));
        //mp4_update_stts_atom duration = 3200, count = 16392
        TSDebug(PLUGIN_NAME,"[mp4_crop_stts_data_start] duration = %u, count = %u", duration,count);
        if (start_time < (uint64_t)count * duration) {
            pass = (uint32_t)(start_time / duration);
            start_sample += pass;

            goto found;//found
        }

        start_sample += count;//计算已经丢弃了多少个sample
        start_time -= (uint64_t)count * duration; //还剩多少时间
        TSIOBufferReaderConsume(readerp, sizeof(mp4_stts_entry)); //丢弃
    }

    found:

    TSIOBufferReaderFree(readerp);

    old_sample = start_sample;//已经检查过的sample
    //到 Sync Sample Box 找最适合的关键帧  返回sample序号
    key_sample = this->mp4_find_key_sample(start_sample, trak); // find the last key frame before start_sample
    TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_end] key_sample= %lu", key_sample);
    if (old_sample != key_sample) {
        start_sample = key_sample - 1;
    }
    TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_start] start_sample= %lu", start_sample);
    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STTS_DATA].reader);

    trak->start_sample = start_sample;//找到start_sample
    //更新time to sample box
    for (i = 0; i < entries; i++) {
        duration = (uint32_t)mp4_reader_get_32value(readerp, offsetof(mp4_stts_entry, duration));
        count    = (uint32_t)mp4_reader_get_32value(readerp, offsetof(mp4_stts_entry, count));

        if (start_sample < count) {
            count -= start_sample;
            mp4_reader_set_32value(readerp, offsetof(mp4_stts_entry, count), count);
            //计算总共丢弃的duration
            sum += (uint64_t)start_sample * duration;
            break;
        }

        start_sample -= count;
        sum += (uint64_t)count * duration;

        TSIOBufferReaderConsume(readerp, sizeof(mp4_stts_entry));
    }

    if (this->rs == 0) {
        //实际时间 0.2s  对应的duration = mdhd.timescale * 0.2s
        // 丢弃了多少时间 ＝ 多个sample ＊ 每个sample等于多少秒 * 1000
        this->rs = ((double)sum / trak->duration) * ((double)trak->duration / trak->timescale) * 1000;
        //mp4_update_stsz_atom rs=2993800.000000, sum=47900800
        TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_start] rs=%lf, sum=%ld", this->rs, sum);
    }

    left = entries - i; //之前遍历，丢弃了，剩下多少数据
    //mp4_update_stts_atom left=2, entries=2
    TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_start] left=%u, entries=%u", left, entries);
    atom_size = sizeof(mp4_stts_atom) + left * sizeof(mp4_stts_entry);
    trak->size += atom_size;//默认位0 开始累加
    //mp4_update_stts_atom trak->size=198
    TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_start] trak->size=%lu, end-start=%ld", trak->size, this->length);
    mp4_reader_set_32value(trak->atoms[MP4_STTS_ATOM].reader, offsetof(mp4_stts_atom, size), atom_size);
    mp4_reader_set_32value(trak->atoms[MP4_STTS_ATOM].reader, offsetof(mp4_stts_atom, entries), left);

    TSIOBufferReaderConsume(trak->atoms[MP4_STTS_DATA].reader, i * sizeof(mp4_stts_entry));
    TSIOBufferReaderFree(readerp);

    TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_start] --------------------------end-------------------------");
    return 0;
}


int Mp4Meta::mp4_crop_stts_data_end(Mp4Trak *trak)
{
    uint32_t i, entries, count, duration, pass;
    uint32_t end_sample, left;
    uint32_t key_sample, old_sample;
    uint64_t end_time, sum;
    int64_t atom_size, avail, copy_avail;
    TSIOBufferReader readerp;

    TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_end] --------------------------start-------------------------");

    if (trak->atoms[MP4_STTS_DATA].buffer == nullptr) {
        return -1;
    }

    sum = 0;

    entries    = trak->time_to_sample_entries;
    //time = duration / timescale      duration = time * timescale / 1000 (将start 毫秒转为秒)
    end_time = this->end * trak->timescale / 1000;
    if (this->end_rs > 0) {
        end_time = (uint64_t)(this->end_rs * trak->timescale / 1000);
    }

    TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_end] end_rs= %lf",this->end_rs);
    //mp4_update_stts_atom start_time = 48000000 entries=2
    TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_end] end_time = %ld entries=%d",end_time, entries);
    end_sample = 0;//开始start_sample
    readerp      = TSIOBufferReaderClone(trak->atoms[MP4_STTS_DATA].reader);

    for (i = 0; i < entries; i++) {//根据时间查找
        duration = (uint32_t)mp4_reader_get_32value(readerp, offsetof(mp4_stts_entry, duration));
        count    = (uint32_t)mp4_reader_get_32value(readerp, offsetof(mp4_stts_entry, count));
        //mp4_update_stts_atom duration = 3200, count = 16392
        TSDebug(PLUGIN_NAME,"[mp4_crop_stts_data_end] duration = %u, count = %u", duration,count);
        if (end_time < (uint64_t)count * duration) {
            pass = (uint32_t)(end_time / duration);
            end_sample += pass;

            goto found;//found
        }

        end_sample += count;//计算保留了多少个sample
        end_time -= (uint64_t)count * duration; //还剩多少时间
        TSIOBufferReaderConsume(readerp, sizeof(mp4_stts_entry)); //丢弃
    }

    found:

    TSIOBufferReaderFree(readerp);

    old_sample = end_sample;//已经检查过的sample
    //到 Sync Sample Box 找最适合的关键帧  返回sample序号
    key_sample = this->mp4_find_key_sample(end_sample, trak); // find the last key frame before start_sample
    TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_end] key_sample= %lu", key_sample);
    if (old_sample != key_sample) {
        end_sample = key_sample -1;
    }
    TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_end] end_sample= %lu", end_sample);
    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STTS_DATA].reader);

    trak->end_sample = end_sample;//找到end_sample
    //更新time to sample box
    for (i = 0; i < entries; i++) {
        duration = (uint32_t)mp4_reader_get_32value(readerp, offsetof(mp4_stts_entry, duration));
        count    = (uint32_t)mp4_reader_get_32value(readerp, offsetof(mp4_stts_entry, count));

        if (end_sample < count) {
            //计算总共保留的duration
            sum += (uint64_t)end_sample * duration;
            break;
        }

        end_sample -= count;
        sum += (uint64_t)count * duration;

        TSIOBufferReaderConsume(readerp, sizeof(mp4_stts_entry));
    }

    if (this->end_rs == 0) {
        //实际时间 0.2s  对应的duration = mdhd.timescale * 0.2s
        // 丢弃了多少时间 ＝ 多个sample ＊ 每个sample等于多少秒 * 1000
        this->end_rs = ((double)sum / trak->duration) * ((double)trak->duration / trak->timescale) * 1000;
        //mp4_update_stsz_atom rs=2993800.000000, sum=47900800
        TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_end] end_rs=%lf, sum=%ld", this->end_rs, sum);
    }

    TSIOBufferReaderFree(readerp);
    TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_end] --------------------------end-------------------------");
    return 0;
}

/**
 * time to sample box
 * size, type, version flags, number of entries
 * entry 1: sample count 1, sample duration 42
 * .......
 * 实际时间 0.2s  对应的duration = mdhd.timescale * 0.2s
 *   entries {u_char count[4],u_char duration[4]}
 */
int
Mp4Meta::mp4_update_stts_atom(Mp4Trak *trak)
{

    if(this->mp4_crop_stts_data_start(trak) < 0)
        return -1;

//    if (this->length > 0) {
//        if(this->mp4_crop_stts_data_end(trak) < 0 )
//            return -1;
//    }


    return 0;
}

//int
//Mp4Meta::mp4_update_stts_atom(Mp4Trak *trak)
//{
//  uint32_t i, entries, count, duration, pass;
//  uint32_t start_sample, left, start_count;
//  uint32_t key_sample, old_sample;
//  uint64_t start_time, sum;
//  int64_t atom_size;
//  TSIOBufferReader readerp;
//
//  if (trak->atoms[MP4_STTS_DATA].buffer == nullptr) {
//    return -1;
//  }
//
//  sum = start_count = 0;
//
//  entries    = trak->time_to_sample_entries;
//    //time = duration / timescale      duration = time * timescale / 1000 (将start 毫秒转为秒)
//  start_time = this->start * trak->timescale / 1000;
//  if (this->rs > 0) {
//    start_time = (uint64_t)(this->rs * trak->timescale / 1000);
//  }
//
//  TSDebug(PLUGIN_NAME, "mp4_update_stts_atom (this->rs > 0)= %lf",this->rs);
//    //mp4_update_stts_atom start_time = 48000000 entries=2
//  TSDebug(PLUGIN_NAME, "mp4_update_stts_atom start_time = %ld entries=%d",start_time, entries);
//  start_sample = 0;//开始start_sample
//  readerp      = TSIOBufferReaderClone(trak->atoms[MP4_STTS_DATA].reader);
//
//  for (i = 0; i < entries; i++) {//根据时间查找
//    duration = (uint32_t)mp4_reader_get_32value(readerp, offsetof(mp4_stts_entry, duration));
//    count    = (uint32_t)mp4_reader_get_32value(readerp, offsetof(mp4_stts_entry, count));
//      //mp4_update_stts_atom duration = 3200, count = 16392
//    TSDebug(PLUGIN_NAME,"mp4_update_stts_atom duration = %u, count = %u", duration,count);
//    if (start_time < (uint64_t)count * duration) {
//      pass = (uint32_t)(start_time / duration);
//      start_sample += pass;
//
//      goto found;//found
//    }
//
//    start_sample += count;//计算已经丢弃了多少个sample
//    start_time -= (uint64_t)count * duration; //还剩多少时间
//    TSIOBufferReaderConsume(readerp, sizeof(mp4_stts_entry)); //丢弃
//  }
//
//found:
//
//  TSIOBufferReaderFree(readerp);
//
//  old_sample = start_sample;//已经检查过的sample
//  //到 Sync Sample Box 找最适合的关键帧  返回sample序号
//  key_sample = this->mp4_find_key_sample(start_sample, trak); // find the last key frame before start_sample
//
//  if (old_sample != key_sample) {
//    start_sample = key_sample - 1;
//  }
//
//  readerp = TSIOBufferReaderClone(trak->atoms[MP4_STTS_DATA].reader);
//
//  trak->start_sample = start_sample;//找到start_sample
//  //更新time to sample box
//  for (i = 0; i < entries; i++) {
//    duration = (uint32_t)mp4_reader_get_32value(readerp, offsetof(mp4_stts_entry, duration));
//    count    = (uint32_t)mp4_reader_get_32value(readerp, offsetof(mp4_stts_entry, count));
//
//    if (start_sample < count) {
//      count -= start_sample;
//      mp4_reader_set_32value(readerp, offsetof(mp4_stts_entry, count), count);
//      //计算总共丢弃的duration
//      sum += (uint64_t)start_sample * duration;
//      break;
//    }
//
//    start_sample -= count;
//    sum += (uint64_t)count * duration;
//
//    TSIOBufferReaderConsume(readerp, sizeof(mp4_stts_entry));
//  }
//
//  if (this->rs == 0) {
//    //实际时间 0.2s  对应的duration = mdhd.timescale * 0.2s
//    // 丢弃了多少时间 ＝ 多个sample ＊ 每个sample等于多少秒 * 1000
//    this->rs = ((double)sum / trak->duration) * ((double)trak->duration / trak->timescale) * 1000;
//      //mp4_update_stsz_atom rs=2993800.000000, sum=47900800
//		TSDebug(PLUGIN_NAME, "mp4_update_stsz_atom rs=%lf, sum=%ld", this->rs, sum);
//  }
//
//  left = entries - i; //之前遍历，丢弃了，剩下多少数据
//    //mp4_update_stts_atom left=2, entries=2
//  TSDebug(PLUGIN_NAME, "mp4_update_stts_atom left=%u, entries=%u", left,entries);
//  atom_size = sizeof(mp4_stts_atom) + left * sizeof(mp4_stts_entry);
//  trak->size += atom_size;//默认位0 开始累加
//    //mp4_update_stts_atom trak->size=198
//  TSDebug(PLUGIN_NAME, "mp4_update_stts_atom trak->size=%lu", trak->size);
//  mp4_reader_set_32value(trak->atoms[MP4_STTS_ATOM].reader, offsetof(mp4_stts_atom, size), atom_size);
//  mp4_reader_set_32value(trak->atoms[MP4_STTS_ATOM].reader, offsetof(mp4_stts_atom, entries), left);
//
//  TSIOBufferReaderConsume(trak->atoms[MP4_STTS_DATA].reader, i * sizeof(mp4_stts_entry));
//  TSIOBufferReaderFree(readerp);
//
//  return 0;
//}


/**
 * Sync Sample Box
 * size, type, version flags, number of entries
 *
 * 该box 决定了整个mp4 文件是否可以拖动，如果box 只有一个entry,则拖拉时，进度到最后
 */
int
Mp4Meta::mp4_update_stss_atom(Mp4Trak *trak)
{
  int64_t atom_size;
  uint32_t i, j, entries, sample, start_sample, left;
  TSIOBufferReader readerp;

  if (trak->atoms[MP4_STSS_DATA].buffer == nullptr) {
    return 0;
  }

  readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSS_DATA].reader);

  start_sample = trak->start_sample + 1;
  entries      = trak->sync_samples_entries;

  for (i = 0; i < entries; i++) {
    sample = (uint32_t)mp4_reader_get_32value(readerp, 0);

    if (sample >= start_sample) {
      goto found;
    }

    TSIOBufferReaderConsume(readerp, sizeof(uint32_t));
  }

  TSIOBufferReaderFree(readerp);
  return -1;

found:

  left = entries - i;

  start_sample = trak->start_sample;
  for (j = 0; j < left; j++) {
    sample = (uint32_t)mp4_reader_get_32value(readerp, 0);
    sample -= start_sample;
    mp4_reader_set_32value(readerp, 0, sample);
    TSIOBufferReaderConsume(readerp, sizeof(uint32_t));
  }

  atom_size = sizeof(mp4_stss_atom) + left * sizeof(uint32_t);
  trak->size += atom_size;
    //mp4_update_stss_atom trak->size=246
  TSDebug(PLUGIN_NAME, "mp4_update_stss_atom trak->size=%lu", trak->size);
  mp4_reader_set_32value(trak->atoms[MP4_STSS_ATOM].reader, offsetof(mp4_stss_atom, size), atom_size);

  mp4_reader_set_32value(trak->atoms[MP4_STSS_ATOM].reader, offsetof(mp4_stss_atom, entries), left);

  TSIOBufferReaderConsume(trak->atoms[MP4_STSS_DATA].reader, i * sizeof(uint32_t));
  TSIOBufferReaderFree(readerp);

  return 0;
}

int
Mp4Meta::mp4_update_ctts_atom(Mp4Trak *trak)
{
  int64_t atom_size;
  uint32_t i, entries, start_sample, left;
  uint32_t count;
  TSIOBufferReader readerp;

  if (trak->atoms[MP4_CTTS_DATA].buffer == nullptr) {
    return 0;
  }

  readerp = TSIOBufferReaderClone(trak->atoms[MP4_CTTS_DATA].reader);

  start_sample = trak->start_sample + 1;
  entries      = trak->composition_offset_entries;

  for (i = 0; i < entries; i++) {
    count = (uint32_t)mp4_reader_get_32value(readerp, offsetof(mp4_ctts_entry, count));

    if (start_sample <= count) {
      count -= (start_sample - 1);
      mp4_reader_set_32value(readerp, offsetof(mp4_ctts_entry, count), count);
      goto found;
    }

    start_sample -= count;
    TSIOBufferReaderConsume(readerp, sizeof(mp4_ctts_entry));
  }

  if (trak->atoms[MP4_CTTS_ATOM].reader) {
    TSIOBufferReaderFree(trak->atoms[MP4_CTTS_ATOM].reader);
    TSIOBufferDestroy(trak->atoms[MP4_CTTS_ATOM].buffer);

    trak->atoms[MP4_CTTS_ATOM].buffer = nullptr;
    trak->atoms[MP4_CTTS_ATOM].reader = nullptr;
  }

  TSIOBufferReaderFree(trak->atoms[MP4_CTTS_DATA].reader);
  TSIOBufferDestroy(trak->atoms[MP4_CTTS_DATA].buffer);

  trak->atoms[MP4_CTTS_DATA].reader = nullptr;
  trak->atoms[MP4_CTTS_DATA].buffer = nullptr;

  TSIOBufferReaderFree(readerp);
  return 0;

found:

  left      = entries - i;
  atom_size = sizeof(mp4_ctts_atom) + left * sizeof(mp4_ctts_entry);
  trak->size += atom_size;
    //mp4_update_ctts_atom trak->size=11414
  TSDebug(PLUGIN_NAME, "mp4_update_ctts_atom trak->size=%lu", trak->size);
  mp4_reader_set_32value(trak->atoms[MP4_CTTS_ATOM].reader, offsetof(mp4_ctts_atom, size), atom_size);
  mp4_reader_set_32value(trak->atoms[MP4_CTTS_ATOM].reader, offsetof(mp4_ctts_atom, entries), left);

  TSIOBufferReaderConsume(trak->atoms[MP4_CTTS_DATA].reader, i * sizeof(mp4_ctts_entry));
  TSIOBufferReaderFree(readerp);

  return 0;
}

int
Mp4Meta::mp4_crop_stsc_data_start(Mp4Trak *trak)
{
    int64_t atom_size;
    uint32_t i, entries, samples, start_sample;
    uint32_t chunk, next_chunk, n, id, j;
    mp4_stsc_entry *first;
    TSIOBufferReader readerp;

    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_start] --------------start--------------");

    if (trak->atoms[MP4_STSC_DATA].buffer == nullptr) {
        return -1;
    }

    if (trak->sample_to_chunk_entries == 0) {
        return -1;
    }

    if (this->end > 0) {
        this->mp4_crop_stsc_data_end(trak);
    }

    start_sample = (uint32_t)trak->start_sample;
    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_start] sample_to_chunk_entries=%lu, start_sample=%lu", trak->sample_to_chunk_entries,start_sample);
    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSC_DATA].reader);

    //entry 1:first chunk 1, samples pre chunk 13, sample description 1 ('self-ref')
    //chunk 1 sample 1 id 1
    //chunk 490 sample 3 id 1
    //第500个sample 500 ＝ 28 ＊ 13 ＋ 12 ＋ 13*9 ＋ 7

    chunk   = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, chunk));
    samples = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, samples));
    id      = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, id));
    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_start] chunk=%lu,samples=%lu,id=%lu", chunk,samples,id);
    TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry));

    for (i = 1; i < trak->sample_to_chunk_entries; i++) {
        next_chunk = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, chunk));

        n = (next_chunk - chunk) * samples;

        if (start_sample <= n) {
            goto found;
        }

        start_sample -= n;

        chunk   = next_chunk;
        samples = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, samples));
        id      = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, id));
        TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_start]---for-- chunk=%lu,samples=%lu,id=%lu", chunk,samples,id);
        TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry));
    }

    next_chunk = trak->chunks;//chunk offset的数目 最后一个chunk

    n = (next_chunk - chunk) * samples; //start_sample 最大就是本身 超过就算出错处理, = 0 就算对了
    if (start_sample > n) {
        TSIOBufferReaderFree(readerp);
        return -1;
    }

    found:

    TSIOBufferReaderFree(readerp);
    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_start] start_sample=%lu, next_chunk=%lu, chunk=%lu,samples=%lu",
            start_sample,next_chunk,chunk,samples);
    entries = trak->sample_to_chunk_entries - i + 1;
    if (samples == 0) {
        return -1;
    }
    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_start] entries=%lu,i=%lu", entries,i);

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSC_DATA].reader);
    TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry) * (i - 1));

    trak->start_chunk = chunk - 1;
    trak->start_chunk += start_sample / samples;
    trak->chunk_samples = start_sample % samples;
    //mp4_update_stsc_atom bbbb start_chunk=14967, chunk_samples=0, i=2
    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_start] start_chunk=%u, chunk_samples=%u, i=%u",
            trak->start_chunk, trak->chunk_samples, i);

    atom_size = sizeof(mp4_stsc_atom) + entries * sizeof(mp4_stsc_entry);

    mp4_reader_set_32value(readerp, offsetof(mp4_stsc_entry, chunk), 1);

    if (trak->chunk_samples && next_chunk - trak->start_chunk == 2) {
        mp4_reader_set_32value(readerp, offsetof(mp4_stsc_entry, samples), samples - trak->chunk_samples);
        TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_start] ==2 samples=%lu", samples - trak->chunk_samples);
    } else if (trak->chunk_samples) {
        first = &trak->stsc_chunk_entry;
        mp4_set_32value(first->chunk, 1);
        mp4_set_32value(first->samples, samples - trak->chunk_samples);
        mp4_set_32value(first->id, id);
        TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_start] else samples=%lu", samples - trak->chunk_samples);
        trak->atoms[MP4_STSC_CHUNK].buffer = TSIOBufferSizedCreate(TS_IOBUFFER_SIZE_INDEX_128);
        trak->atoms[MP4_STSC_CHUNK].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STSC_CHUNK].buffer);
        TSIOBufferWrite(trak->atoms[MP4_STSC_CHUNK].buffer, first, sizeof(mp4_stsc_entry));

        mp4_reader_set_32value(readerp, offsetof(mp4_stsc_entry, chunk), 2);

        entries++;
        TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_start] else entries=%lu", entries);
        atom_size += sizeof(mp4_stsc_entry);
    }

    TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry));
    //mp4_update_stsc_atom sample_to_chunk_entries=2
    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_start] sample_to_chunk_entries=%u, i=%lu",
            trak->sample_to_chunk_entries, i);
    for (j = i; j < trak->sample_to_chunk_entries; j++) {
        chunk = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, chunk));
        chunk -= trak->start_chunk;

        TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_start] chunk-trak->start_chunk=%lu", chunk);
        mp4_reader_set_32value(readerp, offsetof(mp4_stsc_entry, chunk), chunk);
        TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry));
    }

    trak->size += atom_size;
    //mp4_update_stsc_atom trak->size=11442
    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_start] trak->size=%lu", trak->size);
    mp4_reader_set_32value(trak->atoms[MP4_STSC_ATOM].reader, offsetof(mp4_stsc_atom, size), atom_size);
    mp4_reader_set_32value(trak->atoms[MP4_STSC_ATOM].reader, offsetof(mp4_stsc_atom, entries), entries);


    TSIOBufferReaderConsume(trak->atoms[MP4_STSC_DATA].reader, (i - 1) * sizeof(mp4_stsc_entry));
    TSIOBufferReaderFree(readerp);
    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_start] --------------end--------------");
    return 0;
}
int
Mp4Meta::mp4_crop_stsc_data_end(Mp4Trak *trak)
{
    uint32_t i,samples, end_sample;
    uint32_t chunk, next_chunk, n, id;
    TSIOBufferReader readerp;

    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_end] --------------start--------------");

    end_sample = (uint32_t)trak->end_sample;
    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_end] sample_to_chunk_entries=%lu, end_sample=%lu", trak->sample_to_chunk_entries,end_sample);
    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSC_DATA].reader);

    //entry 1:first chunk 1, samples pre chunk 13, sample description 1 ('self-ref')
    //chunk 1 sample 1 id 1
    //chunk 490 sample 3 id 1
    //第500个sample 500 ＝ 28 ＊ 13 ＋ 12 ＋ 13*9 ＋ 7

    chunk   = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, chunk));
    samples = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, samples));
    id      = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, id));
    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_end] chunk=%lu,samples=%lu,id=%lu", chunk,samples,id);
    TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry));

    for (i = 1; i < trak->sample_to_chunk_entries; i++) {
        next_chunk = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, chunk));

        n = (next_chunk - chunk) * samples;

        if (end_sample <= n) {
            goto found;
        }

        end_sample -= n;

        chunk   = next_chunk;
        samples = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, samples));
        id      = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, id));
        TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_end]---for-- chunk=%lu,samples=%lu,id=%lu", chunk,samples,id);
        TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry));
    }

    next_chunk = trak->chunks;//chunk offset的数目 最后一个chunk

    n = (next_chunk - chunk) * samples; //start_sample 最大就是本身 超过就算出错处理, = 0 就算对了
    if (end_sample > n) {
        TSIOBufferReaderFree(readerp);
        return -1;
    }

    found:

    TSIOBufferReaderFree(readerp);
    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_end] end_sample=%lu, next_chunk=%lu, chunk=%lu,samples=%lu",
            end_sample,next_chunk,chunk,samples);
    if (samples == 0) {
        return -1;
    }

    trak->end_chunk = chunk - 1;
    trak->end_chunk += end_sample / samples;
    trak->end_chunk_samples = end_sample % samples;
    //mp4_update_stsc_atom bbbb start_chunk=14967, chunk_samples=0, i=2
    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_end] end_chunk=%u, end_chunk_samples=%u, i=%u",
            trak->end_chunk, trak->end_chunk_samples, i);
    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data_end] --------------end--------------");
    return 0;
}

/**
 * sample to chunk box
 * size, type, version, flags, number of entries
 * entry 1:first chunk 1, samples pre chunk 13, sample description 1 ('self-ref')
 * ...............
 * 第500个sample 500 ＝ 28 ＊ 13 ＋ 12 ＋ 13*9 ＋ 7
 *
 * first_chunk 表示 这一组相同类型的chunk中 的第一个chunk数。
 * 这些chunk 中包含的Sample 数量，即samples_per_chunk 是一致的。
 * 每个Sample 可以通过sample_description_index 去stsd box 找到描述信息。
 */
int
Mp4Meta::mp4_update_stsc_atom(Mp4Trak *trak)
{
    if(this->mp4_crop_stsc_data_start(trak) < 0)
        return -1;

//    if (this->length > 0) {
//        if (this->mp4_crop_stsc_data_end(trak) < 0)
//            return -1;
//    }

    return 0;
}

//int
//Mp4Meta::mp4_update_stsc_atom(Mp4Trak *trak)
//{
//  int64_t atom_size;
//  uint32_t i, entries, samples, start_sample;
//  uint32_t chunk, next_chunk, n, id, j;
//  mp4_stsc_entry *first;
//  TSIOBufferReader readerp;
//
//  if (trak->atoms[MP4_STSC_DATA].buffer == nullptr) {
//    return -1;
//  }
//
//  if (trak->sample_to_chunk_entries == 0) {
//    return -1;
//  }
//
//  start_sample = (uint32_t)trak->start_sample;
//
//  readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSC_DATA].reader);
//
//  //entry 1:first chunk 1, samples pre chunk 13, sample description 1 ('self-ref')
//  //chunk 1 sample 1 id 1
//  //chunk 490 sample 3 id 1
//  //第500个sample 500 ＝ 28 ＊ 13 ＋ 12 ＋ 13*9 ＋ 7
//
//  chunk   = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, chunk));
//  samples = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, samples));
//  id      = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, id));
//
//  TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry));
//
//  for (i = 1; i < trak->sample_to_chunk_entries; i++) {
//    next_chunk = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, chunk));
//
//    n = (next_chunk - chunk) * samples;
//
//    if (start_sample <= n) {
//      goto found;
//    }
//
//    start_sample -= n;
//
//    chunk   = next_chunk;
//    samples = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, samples));
//    id      = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, id));
//
//    TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry));
//  }
//
//  next_chunk = trak->chunks;//chunk offset的数目 最后一个chunk
//
//  n = (next_chunk - chunk) * samples; //start_sample 最大就是本身 超过就算出错处理, = 0 就算对了
//  if (start_sample > n) {
//    TSIOBufferReaderFree(readerp);
//    return -1;
//  }
//
//found:
//
//  TSIOBufferReaderFree(readerp);
//
//  entries = trak->sample_to_chunk_entries - i + 1;
//  if (samples == 0) {
//    return -1;
//  }
//
//  readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSC_DATA].reader);
//  TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry) * (i - 1));
//
//  trak->start_chunk = chunk - 1;
//  trak->start_chunk += start_sample / samples;
//  trak->chunk_samples = start_sample % samples;
//    //mp4_update_stsc_atom bbbb start_chunk=14967, chunk_samples=0, i=2
//	TSDebug(PLUGIN_NAME, "mp4_update_stsc_atom bbbb start_chunk=%u, chunk_samples=%u, i=%u",
//			trak->start_chunk, trak->chunk_samples, i);
//  atom_size = sizeof(mp4_stsc_atom) + entries * sizeof(mp4_stsc_entry);
//
//  mp4_reader_set_32value(readerp, offsetof(mp4_stsc_entry, chunk), 1);
//
//  if (trak->chunk_samples && next_chunk - trak->start_chunk == 2) {
//    mp4_reader_set_32value(readerp, offsetof(mp4_stsc_entry, samples), samples - trak->chunk_samples);
//
//  } else if (trak->chunk_samples) {
//    first = &trak->stsc_chunk_entry;
//    mp4_set_32value(first->chunk, 1);
//    mp4_set_32value(first->samples, samples - trak->chunk_samples);
//    mp4_set_32value(first->id, id);
//
//    trak->atoms[MP4_STSC_CHUNK].buffer = TSIOBufferSizedCreate(TS_IOBUFFER_SIZE_INDEX_128);
//    trak->atoms[MP4_STSC_CHUNK].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STSC_CHUNK].buffer);
//    TSIOBufferWrite(trak->atoms[MP4_STSC_CHUNK].buffer, first, sizeof(mp4_stsc_entry));
//
//    mp4_reader_set_32value(readerp, offsetof(mp4_stsc_entry, chunk), 2);
//
//    entries++;
//    atom_size += sizeof(mp4_stsc_entry);
//  }
//
//  TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry));
//    //mp4_update_stsc_atom sample_to_chunk_entries=2
//	TSDebug(PLUGIN_NAME, "mp4_update_stsc_atom sample_to_chunk_entries=%u",
//			trak->sample_to_chunk_entries);
//  for (j = i; j < trak->sample_to_chunk_entries; j++) {
//    chunk = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, chunk));
//    chunk -= trak->start_chunk;
//    mp4_reader_set_32value(readerp, offsetof(mp4_stsc_entry, chunk), chunk);
//    TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry));
//  }
//
//  trak->size += atom_size;
//    //mp4_update_stsc_atom trak->size=11442
//  TSDebug(PLUGIN_NAME, "mp4_update_stsc_atom trak->size=%lu", trak->size);
//  mp4_reader_set_32value(trak->atoms[MP4_STSC_ATOM].reader, offsetof(mp4_stsc_atom, size), atom_size);
//  mp4_reader_set_32value(trak->atoms[MP4_STSC_ATOM].reader, offsetof(mp4_stsc_atom, entries), entries);
//
//  TSIOBufferReaderConsume(trak->atoms[MP4_STSC_DATA].reader, (i - 1) * sizeof(mp4_stsc_entry));
//  TSIOBufferReaderFree(readerp);
//
//  return 0;
//}


int
Mp4Meta::mp4_crop_stsz_data_start(Mp4Trak *trak)
{
    uint32_t i;
    int64_t atom_size, avail;
    uint32_t pass;
    TSIOBufferReader readerp;

    if (trak->atoms[MP4_STSZ_DATA].buffer == nullptr) {
        return 0;
    }

    if (trak->start_sample > trak->sample_sizes_entries) {
        return -1;
    }

    if (this->end > 0) {
        this->mp4_crop_stsz_data_end(trak);
    }

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSZ_DATA].reader);
    avail   = TSIOBufferReaderAvail(readerp);

    pass = trak->start_sample * sizeof(uint32_t);

    TSIOBufferReaderConsume(readerp, pass - sizeof(uint32_t) * (trak->chunk_samples));

    for (i = 0; i < trak->chunk_samples; i++) {
        trak->chunk_samples_size += mp4_reader_get_32value(readerp, 0);
        TSIOBufferReaderConsume(readerp, sizeof(uint32_t));
    }

    atom_size = sizeof(mp4_stsz_atom) + avail - pass;
    trak->size += atom_size;

    mp4_reader_set_32value(trak->atoms[MP4_STSZ_ATOM].reader, offsetof(mp4_stsz_atom, size), atom_size);
    mp4_reader_set_32value(trak->atoms[MP4_STSZ_ATOM].reader, offsetof(mp4_stsz_atom, entries),
                           trak->sample_sizes_entries - trak->start_sample);

    TSIOBufferReaderConsume(trak->atoms[MP4_STSZ_DATA].reader, pass);
    TSIOBufferReaderFree(readerp);
    return 0;
}
int
Mp4Meta::mp4_crop_stsz_data_end(Mp4Trak *trak)
{
    uint32_t i;
    uint32_t pass;
    TSIOBufferReader readerp;

    if (trak->atoms[MP4_STSZ_DATA].buffer == nullptr) {
        return 0;
    }

    if (trak->end_sample > trak->sample_sizes_entries) {
        return -1;
    }

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSZ_DATA].reader);
    pass = trak->end_sample * sizeof(uint32_t);

    TSIOBufferReaderConsume(readerp, pass - sizeof(uint32_t) * (trak->end_chunk_samples));

    for (i = 0; i < trak->end_chunk_samples; i++) {
        trak->end_chunk_samples_size += mp4_reader_get_32value(readerp, 0);
        TSIOBufferReaderConsume(readerp, sizeof(uint32_t));
    }

    TSIOBufferReaderFree(readerp);
    return 0;
}

int
Mp4Meta::mp4_update_stsz_atom(Mp4Trak *trak)
{
    if(this->mp4_crop_stsz_data_start(trak) < 0)
        return -1;
    return 0;
}

//int
//Mp4Meta::mp4_update_stsz_atom(Mp4Trak *trak)
//{
//  uint32_t i;
//  int64_t atom_size, avail;
//  uint32_t pass;
//  TSIOBufferReader readerp;
//
//  if (trak->atoms[MP4_STSZ_DATA].buffer == nullptr) {
//    return 0;
//  }
//
//  if (trak->start_sample > trak->sample_sizes_entries) {
//    return -1;
//  }
//
//  readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSZ_DATA].reader);
//  avail   = TSIOBufferReaderAvail(readerp);
//
//  pass = trak->start_sample * sizeof(uint32_t);
//
//  TSIOBufferReaderConsume(readerp, pass - sizeof(uint32_t) * (trak->chunk_samples));
//
//  for (i = 0; i < trak->chunk_samples; i++) {
//    trak->chunk_samples_size += mp4_reader_get_32value(readerp, 0);
//    TSIOBufferReaderConsume(readerp, sizeof(uint32_t));
//  }
//
//  atom_size = sizeof(mp4_stsz_atom) + avail - pass;
//  trak->size += atom_size;
//
//  mp4_reader_set_32value(trak->atoms[MP4_STSZ_ATOM].reader, offsetof(mp4_stsz_atom, size), atom_size);
//  mp4_reader_set_32value(trak->atoms[MP4_STSZ_ATOM].reader, offsetof(mp4_stsz_atom, entries),
//                         trak->sample_sizes_entries - trak->start_sample);
//
//  TSIOBufferReaderConsume(trak->atoms[MP4_STSZ_DATA].reader, pass);
//  TSIOBufferReaderFree(readerp);
//
//  return 0;
//}


int
Mp4Meta::mp4_crop_co64_data_start(Mp4Trak *trak)
{
    int64_t atom_size, avail, pass;
    TSIOBufferReader readerp;

    if (trak->atoms[MP4_CO64_DATA].buffer == nullptr) {
        return -1;
    }

    if (trak->start_chunk > trak->chunks) {
        return -1;
    }

    if(this->end > 0) {
        this->mp4_crop_co64_data_end(trak);
    }

    readerp = trak->atoms[MP4_CO64_DATA].reader;
    avail   = TSIOBufferReaderAvail(readerp);

    pass      = trak->start_chunk * sizeof(uint64_t);
    atom_size = sizeof(mp4_co64_atom) + avail - pass;
    trak->size += atom_size;
    TSDebug(PLUGIN_NAME, "mp4_update_co64_atom trak->size=%lu", trak->size);
    TSIOBufferReaderConsume(readerp, pass);
    trak->start_offset = mp4_reader_get_64value(readerp, 0);
    trak->start_offset += trak->chunk_samples_size;
    mp4_reader_set_64value(readerp, 0, trak->start_offset);

    mp4_reader_set_32value(trak->atoms[MP4_CO64_ATOM].reader, offsetof(mp4_co64_atom, size), atom_size);
    mp4_reader_set_32value(trak->atoms[MP4_CO64_ATOM].reader, offsetof(mp4_co64_atom, entries), trak->chunks - trak->start_chunk);
    TSIOBufferReaderFree(readerp);
    return 0;
}
int
Mp4Meta::mp4_crop_co64_data_end(Mp4Trak *trak)
{
    int64_t atom_size, avail, pass;
    TSIOBufferReader readerp;

    if (trak->atoms[MP4_CO64_DATA].buffer == nullptr) {
        return -1;
    }

    if (trak->end_chunk > trak->chunks) {
        return -1;
    }

    readerp = trak->atoms[MP4_CO64_DATA].reader;
    avail   = TSIOBufferReaderAvail(readerp);

    pass      = trak->end_chunk * sizeof(uint64_t);

    TSIOBufferReaderConsume(readerp, pass);
    trak->end_offset = mp4_reader_get_64value(readerp, 0);
    trak->end_offset += trak->end_chunk_samples_size;
    TSIOBufferReaderFree(readerp);
    return 0;
}

int
Mp4Meta::mp4_update_co64_atom(Mp4Trak *trak)
{
    if(this->mp4_crop_co64_data_start(trak) < 0)
        return -1;
    return 0;
}

//int
//Mp4Meta::mp4_update_co64_atom(Mp4Trak *trak)
//{
//  int64_t atom_size, avail, pass;
//  TSIOBufferReader readerp;
//
//  if (trak->atoms[MP4_CO64_DATA].buffer == nullptr) {
//    return -1;
//  }
//
//  if (trak->start_chunk > trak->chunks) {
//    return -1;
//  }
//
//  readerp = trak->atoms[MP4_CO64_DATA].reader;
//  avail   = TSIOBufferReaderAvail(readerp);
//
//  pass      = trak->start_chunk * sizeof(uint64_t);
//  atom_size = sizeof(mp4_co64_atom) + avail - pass;
//  trak->size += atom_size;
//  TSDebug(PLUGIN_NAME, "mp4_update_co64_atom trak->size=%lu", trak->size);
//  TSIOBufferReaderConsume(readerp, pass);
//  trak->start_offset = mp4_reader_get_64value(readerp, 0);
//  trak->start_offset += trak->chunk_samples_size;
//  mp4_reader_set_64value(readerp, 0, trak->start_offset);
//
//  mp4_reader_set_32value(trak->atoms[MP4_CO64_ATOM].reader, offsetof(mp4_co64_atom, size), atom_size);
//  mp4_reader_set_32value(trak->atoms[MP4_CO64_ATOM].reader, offsetof(mp4_co64_atom, entries), trak->chunks - trak->start_chunk);
//
//  return 0;
//}


int
Mp4Meta::mp4_crop_stco_data_start(Mp4Trak *trak)
{
    int64_t atom_size, avail;
    uint32_t pass;
    TSIOBufferReader readerp;

    if (trak->atoms[MP4_STCO_DATA].buffer == nullptr) {
        return -1;
    }

    if (trak->start_chunk > trak->chunks) {
        return -1;
    }

    if(this->end > 0) {
        this->mp4_crop_stco_data_end(trak);
    }

    readerp = trak->atoms[MP4_STCO_DATA].reader;
    avail   = TSIOBufferReaderAvail(readerp);

    pass      = trak->start_chunk * sizeof(uint32_t);
    atom_size = sizeof(mp4_stco_atom) + avail - pass;
    trak->size += atom_size;

    //丢弃chunk
    TSIOBufferReaderConsume(readerp, pass);

    //首先定位到chunk
    trak->start_offset = mp4_reader_get_32value(readerp, 0);
    //然后再定位到具体的chunk 里的 sample
    trak->start_offset += trak->chunk_samples_size;
    mp4_reader_set_32value(readerp, 0, trak->start_offset);

    mp4_reader_set_32value(trak->atoms[MP4_STCO_ATOM].reader, offsetof(mp4_stco_atom, size), atom_size);
    mp4_reader_set_32value(trak->atoms[MP4_STCO_ATOM].reader, offsetof(mp4_stco_atom, entries), trak->chunks - trak->start_chunk);
//mp4_update_stco_atom start_offset=80012616 chunks=16391, start_chunk=14967
    TSDebug(PLUGIN_NAME, "mp4_update_stco_atom start_offset=%ld chunks=%d, start_chunk=%d",
            trak->start_offset, trak->chunks, trak->start_chunk);
    TSIOBufferReaderFree(readerp);

    return 0;
}
int
Mp4Meta::mp4_crop_stco_data_end(Mp4Trak *trak)
{
    int64_t atom_size, avail;
    uint32_t pass;
    TSIOBufferReader readerp;

    if (trak->atoms[MP4_STCO_DATA].buffer == nullptr) {
        return -1;
    }

    if (trak->end_chunk > trak->chunks) {
        return -1;
    }

    readerp = trak->atoms[MP4_STCO_DATA].reader;
    avail   = TSIOBufferReaderAvail(readerp);

    pass      = trak->end_chunk * sizeof(uint32_t);

    //丢弃chunk
    TSIOBufferReaderConsume(readerp, pass);

    //首先定位到chunk
    trak->end_offset = mp4_reader_get_32value(readerp, 0);
    //然后再定位到具体的chunk 里的 sample
    trak->end_offset += trak->end_chunk_samples_size;
    TSIOBufferReaderFree(readerp);
    return 0;
}

int//chunk offset box 定义了每个chunk在流媒体中的位置
Mp4Meta::mp4_update_stco_atom(Mp4Trak *trak)
{
    if(this->mp4_crop_stco_data_start(trak) < 0)
        return -1;
    return 0;
}

//int//chunk offset box 定义了每个chunk在流媒体中的位置
//Mp4Meta::mp4_update_stco_atom(Mp4Trak *trak)
//{
//  int64_t atom_size, avail;
//  uint32_t pass;
//  TSIOBufferReader readerp;
//
//  if (trak->atoms[MP4_STCO_DATA].buffer == nullptr) {
//    return -1;
//  }
//
//  if (trak->start_chunk > trak->chunks) {
//    return -1;
//  }
//
//  readerp = trak->atoms[MP4_STCO_DATA].reader;
//  avail   = TSIOBufferReaderAvail(readerp);
//
//  pass      = trak->start_chunk * sizeof(uint32_t);
//  atom_size = sizeof(mp4_stco_atom) + avail - pass;
//  trak->size += atom_size;
//
//  //丢弃chunk
//  TSIOBufferReaderConsume(readerp, pass);
//
//  //首先定位到chunk
//  trak->start_offset = mp4_reader_get_32value(readerp, 0);
//  //然后再定位到具体的chunk 里的 sample
//  trak->start_offset += trak->chunk_samples_size;
//  mp4_reader_set_32value(readerp, 0, trak->start_offset);
//
//  mp4_reader_set_32value(trak->atoms[MP4_STCO_ATOM].reader, offsetof(mp4_stco_atom, size), atom_size);
//  mp4_reader_set_32value(trak->atoms[MP4_STCO_ATOM].reader, offsetof(mp4_stco_atom, entries), trak->chunks - trak->start_chunk);
////mp4_update_stco_atom start_offset=80012616 chunks=16391, start_chunk=14967
//  TSDebug(PLUGIN_NAME, "mp4_update_stco_atom start_offset=%ld chunks=%d, start_chunk=%d",
//			trak->start_offset, trak->chunks, trak->start_chunk);
//
//  return 0;
//}

int
Mp4Meta::mp4_update_stbl_atom(Mp4Trak *trak)
{
  trak->size += sizeof(mp4_atom_header);
  mp4_reader_set_32value(trak->atoms[MP4_STBL_ATOM].reader, 0, trak->size);

  return 0;
}

int
Mp4Meta::mp4_update_minf_atom(Mp4Trak *trak)
{
  trak->size += sizeof(mp4_atom_header) + trak->vmhd_size + trak->smhd_size + trak->dinf_size;

  mp4_reader_set_32value(trak->atoms[MP4_MINF_ATOM].reader, 0, trak->size);

  return 0;
}

int
Mp4Meta::mp4_update_mdia_atom(Mp4Trak *trak)
{
  trak->size += sizeof(mp4_atom_header);
  mp4_reader_set_32value(trak->atoms[MP4_MDIA_ATOM].reader, 0, trak->size);

  return 0;
}

int
Mp4Meta::mp4_update_trak_atom(Mp4Trak *trak)
{
  trak->size += sizeof(mp4_atom_header);
  mp4_reader_set_32value(trak->atoms[MP4_TRAK_ATOM].reader, 0, trak->size);

  return 0;
}

int
Mp4Meta::mp4_adjust_co64_atom(Mp4Trak *trak, off_t adjustment)
{
  int64_t pos, avail, offset;
  TSIOBufferReader readerp;

  readerp = TSIOBufferReaderClone(trak->atoms[MP4_CO64_DATA].reader);
  avail   = TSIOBufferReaderAvail(readerp);

  for (pos = 0; pos < avail; pos += sizeof(uint64_t)) {
    offset = mp4_reader_get_64value(readerp, 0);
    offset += adjustment;
    mp4_reader_set_64value(readerp, 0, offset);
    TSIOBufferReaderConsume(readerp, sizeof(uint64_t));
  }

  TSIOBufferReaderFree(readerp);

  return 0;
}

int
Mp4Meta::mp4_adjust_stco_atom(Mp4Trak *trak, int32_t adjustment)
{
  int64_t pos, avail, offset;
  TSIOBufferReader readerp;

  readerp = TSIOBufferReaderClone(trak->atoms[MP4_STCO_DATA].reader);
  avail   = TSIOBufferReaderAvail(readerp);

  for (pos = 0; pos < avail; pos += sizeof(uint32_t)) {
    offset = mp4_reader_get_32value(readerp, 0);
    offset += adjustment;
    mp4_reader_set_32value(readerp, 0, offset);
    TSIOBufferReaderConsume(readerp, sizeof(uint32_t));
  }

  TSIOBufferReaderFree(readerp);

  return 0;
}

int64_t
Mp4Meta::mp4_update_mdat_atom(int64_t start_offset, int64_t end_offset)
{
  int64_t atom_data_size;
  int64_t atom_size;
  int64_t atom_header_size;
  u_char *atom_header;

  atom_data_size  = end_offset > start_offset ? end_offset - start_offset:this->cl - start_offset;//剩余的都是mdat
  this->start_pos = start_offset;
  this->end_pos = end_offset;
  TSDebug(PLUGIN_NAME, "[mp4_update_mdat_atom] this->start_pos= %ld, atom_data_size=%ld", this->start_pos,atom_data_size);
    TSDebug(PLUGIN_NAME, "[mp4_update_mdat_atom] this->end_pos= %ld", this->end_pos);
  atom_header = mdat_atom_header;

  if (atom_data_size > 0xffffffff) {
    atom_size        = 1;
    atom_header_size = sizeof(mp4_atom_header64);
    mp4_set_64value(atom_header + sizeof(mp4_atom_header), sizeof(mp4_atom_header64) + atom_data_size);

  } else {
    atom_size        = sizeof(mp4_atom_header) + atom_data_size;
    atom_header_size = sizeof(mp4_atom_header);
  }

  this->content_length += atom_header_size + atom_data_size;
//    this->content_length += atom_header_size + 1024*1024*1;

	TSDebug(PLUGIN_NAME,"[mp4_update_mdat_atom] atom_header_size=%ld content_length=%ld",
				atom_header_size, this->content_length);
  mp4_set_32value(atom_header, atom_size);
  mp4_set_atom_name(atom_header, 'm', 'd', 'a', 't');

  mdat_atom.buffer = TSIOBufferSizedCreate(TS_IOBUFFER_SIZE_INDEX_128);
  mdat_atom.reader = TSIOBufferReaderAlloc(mdat_atom.buffer);

  TSIOBufferWrite(mdat_atom.buffer, atom_header, atom_header_size);

  return atom_header_size;
}

/**
 * Sync Sample Box
 * size, type, version flags, number of entries
 *
 * 该box 决定了整个mp4 文件是否可以拖动，如果box 只有一个entry,则拖拉时，进度到最后
 */
uint32_t
Mp4Meta::mp4_find_key_sample(uint32_t start_sample, Mp4Trak *trak)
{
  uint32_t i;
  uint32_t sample, prev_sample, entries;
  TSIOBufferReader readerp;

  if (trak->atoms[MP4_STSS_DATA].buffer == nullptr) {
    return start_sample;
  }

  prev_sample = 1;
  entries     = trak->sync_samples_entries;
    //mp4_find_key_sample sync_samples_entries=75
  TSDebug(PLUGIN_NAME, "mp4_find_key_sample sync_samples_entries=%u", entries);
  readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSS_DATA].reader);

  for (i = 0; i < entries; i++) {
    sample = (uint32_t)mp4_reader_get_32value(readerp, 0);
      //mp4_find_key_sample sample=1
      // .....
      //mp4_find_key_sample sample=251
    TSDebug(PLUGIN_NAME, "mp4_find_key_sample sample=%u", sample);
    if (sample > start_sample) {
      goto found;
    }

    prev_sample = sample;
    TSIOBufferReaderConsume(readerp, sizeof(uint32_t));
  }

found:

  TSIOBufferReaderFree(readerp);
  return prev_sample;
}

void
Mp4Meta::mp4_update_mvhd_duration()
{
  int64_t need;
  uint64_t duration, cut, end_cut;
  mp4_mvhd_atom *mvhd;
  mp4_mvhd64_atom mvhd64;

  need = TSIOBufferReaderAvail(mvhd_atom.reader);

  if (need > (int64_t)sizeof(mp4_mvhd64_atom)) {
    need = sizeof(mp4_mvhd64_atom);
  }

  memset(&mvhd64, 0, sizeof(mvhd64));
  IOBufferReaderCopy(mvhd_atom.reader, &mvhd64, need);
  mvhd = (mp4_mvhd_atom *)&mvhd64;

  if (this->rs > 0) {
    cut = (uint64_t)(this->rs * this->timescale / 1000);

  } else {
    cut = this->start * this->timescale / 1000;
  }

    end_cut = 0;
    if(this->end > 0) {
        if (this->end_rs > 0) {
            end_cut = (uint64_t)(this->end_rs * this->timescale / 1000);

        } else {
            end_cut = this->end * this->timescale / 1000;
        }
    }

  if (mvhd->version[0] == 0) {
    duration = mp4_get_32value(mvhd->duration);
      if(this->end > 0 && end_cut > 0 && end_cut > cut) {
          duration = end_cut - cut;
      } else {
          duration -= cut;
      }
    mp4_reader_set_32value(mvhd_atom.reader, offsetof(mp4_mvhd_atom, duration), duration);

  } else { // 64-bit duration
    duration = mp4_get_64value(mvhd64.duration);
      if(this->end > 0 && end_cut > 0 && end_cut > cut) {
          duration = end_cut - cut;
      } else {
          duration -= cut;
      }
    mp4_reader_set_64value(mvhd_atom.reader, offsetof(mp4_mvhd64_atom, duration), duration);
  }
}

void
Mp4Meta::mp4_update_tkhd_duration(Mp4Trak *trak)
{
  int64_t need, cut, end_cut;
  mp4_tkhd_atom *tkhd_atom;
  mp4_tkhd64_atom tkhd64_atom;
  int64_t duration;

  need = TSIOBufferReaderAvail(trak->atoms[MP4_TKHD_ATOM].reader);

  if (need > (int64_t)sizeof(mp4_tkhd64_atom)) {
    need = sizeof(mp4_tkhd64_atom);
  }

  memset(&tkhd64_atom, 0, sizeof(tkhd64_atom));
  IOBufferReaderCopy(trak->atoms[MP4_TKHD_ATOM].reader, &tkhd64_atom, need);
  tkhd_atom = (mp4_tkhd_atom *)&tkhd64_atom;

  if (this->rs > 0) {
    cut = (uint64_t)(this->rs * this->timescale / 1000);

  } else {
    cut = this->start * this->timescale / 1000;
  }
    end_cut = 0;
    if(this->end > 0) {
        if (this->end_rs > 0) {
            end_cut = (uint64_t)(this->end_rs * this->timescale / 1000);

        } else {
            end_cut = this->end * this->timescale / 1000;
        }
    }

  if (tkhd_atom->version[0] == 0) {
    duration = mp4_get_32value(tkhd_atom->duration);
      if(this->end > 0 && end_cut > 0 && end_cut > cut) {
          duration = end_cut - cut;
      } else {
          duration -= cut;
      }

    mp4_reader_set_32value(trak->atoms[MP4_TKHD_ATOM].reader, offsetof(mp4_tkhd_atom, duration), duration);

  } else {
    duration = mp4_get_64value(tkhd64_atom.duration);
      if(this->end > 0 && end_cut > 0 && end_cut > cut) {
          duration = end_cut - cut;
      } else {
          duration -= cut;
      }
    mp4_reader_set_64value(trak->atoms[MP4_TKHD_ATOM].reader, offsetof(mp4_tkhd64_atom, duration), duration);
  }
}

void
Mp4Meta::mp4_update_mdhd_duration(Mp4Trak *trak)
{
  int64_t duration, need, cut, end_cut;
  mp4_mdhd_atom *mdhd;
  mp4_mdhd64_atom mdhd64;

  memset(&mdhd64, 0, sizeof(mp4_mdhd64_atom));

  need = TSIOBufferReaderAvail(trak->atoms[MP4_MDHD_ATOM].reader);

  if (need > (int64_t)sizeof(mp4_mdhd64_atom)) {
    need = sizeof(mp4_mdhd64_atom);
  }

  IOBufferReaderCopy(trak->atoms[MP4_MDHD_ATOM].reader, &mdhd64, need);
  mdhd = (mp4_mdhd_atom *)&mdhd64;

  if (this->rs > 0) {
    cut = (uint64_t)(this->rs * trak->timescale / 1000);
  } else {
    cut = this->start * trak->timescale / 1000;
  }

    end_cut = 0;
    end_cut = 0;
    if(this->end > 0) {
        if (this->end_rs > 0) {
            end_cut = (uint64_t)(this->end_rs * this->timescale / 1000);

        } else {
            end_cut = this->end * this->timescale / 1000;
        }
    }

  if (mdhd->version[0] == 0) {
    duration = mp4_get_32value(mdhd->duration);
      if(this->end > 0 && end_cut > 0 && end_cut > cut) {
          duration = end_cut - cut;
      } else {
          duration -= cut;
      }
    mp4_reader_set_32value(trak->atoms[MP4_MDHD_ATOM].reader, offsetof(mp4_mdhd_atom, duration), duration);
  } else {
    duration = mp4_get_64value(mdhd64.duration);
      if(this->end > 0 && end_cut > 0 && end_cut > cut) {
          duration = end_cut - cut;
      } else {
          duration -= cut;
      }
    mp4_reader_set_64value(trak->atoms[MP4_MDHD_ATOM].reader, offsetof(mp4_mdhd64_atom, duration), duration);
  }
}

static void
mp4_reader_set_32value(TSIOBufferReader readerp, int64_t offset, uint32_t n)
{
  int pos;
  int64_t avail, left;
  TSIOBufferBlock blk;
  const char *start;
  u_char *ptr;

  pos = 0;
  blk = TSIOBufferReaderStart(readerp);

  while (blk) {
    start = TSIOBufferBlockReadStart(blk, readerp, &avail);

    if (avail <= offset) {
      offset -= avail;

    } else {
      left = avail - offset;
      ptr  = (u_char *)(const_cast<char *>(start) + offset);

      while (pos < 4 && left > 0) {
        *ptr++ = (u_char)((n) >> ((3 - pos) * 8));
        pos++;
        left--;
      }

      if (pos >= 4) {
        return;
      }

      offset = 0;
    }

    blk = TSIOBufferBlockNext(blk);
  }
}

static void
mp4_reader_set_64value(TSIOBufferReader readerp, int64_t offset, uint64_t n)
{
  int pos;
  int64_t avail, left;
  TSIOBufferBlock blk;
  const char *start;
  u_char *ptr;

  pos = 0;
  blk = TSIOBufferReaderStart(readerp);

  while (blk) {
    start = TSIOBufferBlockReadStart(blk, readerp, &avail);

    if (avail <= offset) {
      offset -= avail;

    } else {
      left = avail - offset;
      ptr  = (u_char *)(const_cast<char *>(start) + offset);

      while (pos < 8 && left > 0) {
        *ptr++ = (u_char)((n) >> ((7 - pos) * 8));
        pos++;
        left--;
      }

      if (pos >= 4) {
        return;
      }

      offset = 0;
    }

    blk = TSIOBufferBlockNext(blk);
  }
}

static uint32_t
mp4_reader_get_32value(TSIOBufferReader readerp, int64_t offset)
{
  int pos;
  int64_t avail, left;
  TSIOBufferBlock blk;
  const char *start;
  const u_char *ptr;
  u_char res[4];

  pos = 0;
  blk = TSIOBufferReaderStart(readerp);

  while (blk) {
    start = TSIOBufferBlockReadStart(blk, readerp, &avail);

    if (avail <= offset) {
      offset -= avail;

    } else {
      left = avail - offset;
      ptr  = (u_char *)(start + offset);

      while (pos < 4 && left > 0) {
        res[3 - pos] = *ptr++;
        pos++;
        left--;
      }

      if (pos >= 4) {
        return *(uint32_t *)res;
      }

      offset = 0;
    }

    blk = TSIOBufferBlockNext(blk);
  }

  return -1;
}

static uint64_t
mp4_reader_get_64value(TSIOBufferReader readerp, int64_t offset)
{
  int pos;
  int64_t avail, left;
  TSIOBufferBlock blk;
  const char *start;
  u_char *ptr;
  u_char res[8];

  pos = 0;
  blk = TSIOBufferReaderStart(readerp);

  while (blk) {
    start = TSIOBufferBlockReadStart(blk, readerp, &avail);

    if (avail <= offset) {
      offset -= avail;

    } else {
      left = avail - offset;
      ptr  = (u_char *)(start + offset);

      while (pos < 8 && left > 0) {
        res[7 - pos] = *ptr++;
        pos++;
        left--;
      }

      if (pos >= 8) {
        return *(uint64_t *)res;
      }

      offset = 0;
    }

    blk = TSIOBufferBlockNext(blk);
  }

  return -1;
}

static int64_t
IOBufferReaderCopy(TSIOBufferReader readerp, void *buf, int64_t length)
{
  int64_t avail, need, n;
  const char *start;
  TSIOBufferBlock blk;

  n   = 0;
  blk = TSIOBufferReaderStart(readerp);

  while (blk) {
    start = TSIOBufferBlockReadStart(blk, readerp, &avail);
    need  = length < avail ? length : avail;

    if (need > 0) {
      memcpy((char *)buf + n, start, need);
      length -= need;
      n += need;
    }

    if (length == 0) {
      break;
    }

    blk = TSIOBufferBlockNext(blk);
  }

  return n;
}
