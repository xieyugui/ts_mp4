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

static mp4_atom_handler mp4_atoms[] = {{"ftyp",  &Mp4Meta::mp4_read_ftyp_atom},//表明文件类型
                                       {"moov",  &Mp4Meta::mp4_read_moov_atom},//包含了媒体metadata信息,包含1个“mvhd”和若干个“trak”,子box
                                       {"mdat",  &Mp4Meta::mp4_read_mdat_atom},//存放了媒体数据
                                       {nullptr, nullptr}};

static mp4_atom_handler mp4_moov_atoms[] = {{"mvhd",  &Mp4Meta::mp4_read_mvhd_atom},//文件总体信息，如时长，创建时间等
                                            {"trak",  &Mp4Meta::mp4_read_trak_atom},//存放视频，音频的容器  包括video trak,audio trak
                                            {"cmov",  &Mp4Meta::mp4_read_cmov_atom},//
                                            {nullptr, nullptr}};

static mp4_atom_handler mp4_trak_atoms[] = {{"tkhd",  &Mp4Meta::mp4_read_tkhd_atom},//track的总体信息，如时长，高宽等
                                            {"mdia",  &Mp4Meta::mp4_read_mdia_atom},//定义了track媒体类型以及sample数据，描述sample信息
                                            {nullptr, nullptr}};

static mp4_atom_handler mp4_mdia_atoms[] = {{"mdhd",  &Mp4Meta::mp4_read_mdhd_atom},//定义了timescale,trak需要通过timescale换算成真实时间
                                            {"hdlr",  &Mp4Meta::mp4_read_hdlr_atom},//表明trak类型，是video/audio/hint
                                            {"minf",  &Mp4Meta::mp4_read_minf_atom},//数据在子box中
                                            {nullptr, nullptr}};

static mp4_atom_handler mp4_minf_atoms[] = {{"vmhd",  &Mp4Meta::mp4_read_vmhd_atom},//
                                            {"smhd",  &Mp4Meta::mp4_read_smhd_atom},
                                            {"dinf",  &Mp4Meta::mp4_read_dinf_atom},
                                            {"stbl",  &Mp4Meta::mp4_read_stbl_atom},//sample table box存放是时间/偏移的映射关系表
                                            {nullptr, nullptr}};

static mp4_atom_handler mp4_stbl_atoms[] = {
        {"stsd",  &Mp4Meta::mp4_read_stsd_atom},
        {"stts",  &Mp4Meta::mp4_read_stts_atom},// time to sample, 时间戳—sample序号 映射表
        {"stss",  &Mp4Meta::mp4_read_stss_atom},//确定media中的关键帧
        {"ctts",  &Mp4Meta::mp4_read_ctts_atom},
        {"stsc",  &Mp4Meta::mp4_read_stsc_atom},// sample to chunk, sample 和chunk 映射表
        {"stsz",  &Mp4Meta::mp4_read_stsz_atom},// sample size , 每个sample 大小
        {"stco",  &Mp4Meta::mp4_read_stco_atom},//chunk offset, 每个chunk的偏移，sample的偏移可根据其他box推算出来
        {"co64",  &Mp4Meta::mp4_read_co64_atom},//64-bit chunk offseet
        {nullptr, nullptr}};

static void mp4_reader_set_32value(TSIOBufferReader readerp, int64_t offset, uint32_t n);

static void mp4_reader_set_64value(TSIOBufferReader readerp, int64_t offset, uint64_t n);

static uint32_t mp4_reader_get_32value(TSIOBufferReader readerp, int64_t offset);

static uint64_t mp4_reader_get_64value(TSIOBufferReader readerp, int64_t offset);

static int64_t IOBufferReaderCopy(TSIOBufferReader readerp, void *buf, int64_t length);

int
Mp4Meta::parse_meta(bool body_complete) {
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
Mp4Meta::mp4_meta_consume(int64_t size) {
    TSIOBufferReaderConsume(meta_reader, size);
    meta_avail -= size;
    passed += size;
}

int//开始进行moov box 修改
Mp4Meta::post_process_meta() {
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
        TSDebug(PLUGIN_NAME, "[post_process_meta] 0 trak->size= %ld", trak->size);
        if (mp4_update_stts_atom(trak) != 0) {
            return -1;
        }
        TSDebug(PLUGIN_NAME, "[post_process_meta] 1 trak->size= %ld", trak->size);//175
        if (mp4_update_stss_atom(trak) != 0) {
            return -1;
        }
        TSDebug(PLUGIN_NAME, "[post_process_meta] 2 trak->size= %ld", trak->size);//319
        mp4_update_ctts_atom(trak);
        TSDebug(PLUGIN_NAME, "[post_process_meta] 3 trak->size= %ld", trak->size);//18887
        if (mp4_update_stsc_atom(trak) != 0) {
            return -1;
        }
        TSDebug(PLUGIN_NAME, "[post_process_meta] 4 trak->size= %ld", trak->size);//18927
        if (mp4_update_stsz_atom(trak) != 0) {
            return -1;
        }
        TSDebug(PLUGIN_NAME, "[post_process_meta] 5 trak->size= %ld", trak->size);//28947
        if (trak->atoms[MP4_CO64_DATA].buffer) {
            if (mp4_update_co64_atom(trak) != 0) {
                return -1;
            }

        } else if (mp4_update_stco_atom(trak) != 0) {
            return -1;
        }
        TSDebug(PLUGIN_NAME, "[post_process_meta] 6 trak->size= %ld", trak->size);//
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

        if (end_offset < trak->end_offset) {
            end_offset = trak->end_offset;
            if (start_offset > end_offset)
                end_offset = 0;
        }
        TSDebug(PLUGIN_NAME, "[post_process_meta] start_offset = %ld, end_offset=%ld", start_offset, end_offset);

        TSDebug(PLUGIN_NAME, "[post_process_meta] atom avail MP4_LAST_ATOM=%d", MP4_LAST_ATOM);
        for (j = 0; j <= MP4_LAST_ATOM; j++) {
            TSDebug(PLUGIN_NAME, "[post_process_meta] atom avail j=%d", j);
            if (trak->atoms[j].buffer) {
                int64_t a = TSIOBufferReaderAvail(trak->atoms[j].reader);
                TSDebug(PLUGIN_NAME, "[post_process_meta] atom avail a=%lld, j=%d", a, j);
                TSIOBufferCopy(out_handle.buffer, trak->atoms[j].reader, TSIOBufferReaderAvail(trak->atoms[j].reader),
                               0);
            }
        }

//        mp4_update_tkhd_duration(trak);//更新duration
//        mp4_update_mdhd_duration(trak);//更新duration
    }

    if (end_offset < start_offset) {
        end_offset = start_offset;
    }

    this->moov_size += 8;//加上本身的 size + name 大小

    mp4_reader_set_32value(moov_atom.reader, 0, this->moov_size);
    this->content_length += this->moov_size;// content_length = ftype+ moov size
    // content_length= 39840, moov_size=39808
    TSDebug(PLUGIN_NAME, "[post_process_meta] content_length= %ld, moov_size=%ld", this->content_length,
            this->moov_size);
    //this->content_length + (cl-start_offset)的长度 + mdat header size
    //为一个负数，丢弃了多少字节
    //adjustment=-23640769,ftyp=32, moov_size=39808, start_offset= 23680617, mdat_header=8  end_offset= 48723014
    adjustment = this->ftyp_size + this->moov_size + mp4_update_mdat_atom(start_offset, end_offset) - start_offset;
    TSDebug(PLUGIN_NAME,
            "[post_process_meta] adjustment=%ld,ftyp=%ld, moov_size=%ld, start_offset= %ld, mdat_header=%ld",
            adjustment, this->ftyp_size, this->moov_size, start_offset,
            (start_offset + adjustment - this->ftyp_size - this->moov_size));
    TSDebug(PLUGIN_NAME, "[post_process_meta] mdat_atom.reader=%lld", TSIOBufferReaderAvail(mdat_atom.reader));
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

//    mp4_update_mvhd_duration();//更新duration
    TSDebug(PLUGIN_NAME, "[post_process_meta] last  content_length= %ld", this->content_length);
    return 0;
}

/*
 * -1: error
 *  0: unfinished
 *  1: success.
 */
int
Mp4Meta::parse_root_atoms() {
    int i, ret, rc;
    int64_t atom_size, atom_header_size, copied_size;
    char buf[64];
    char *atom_header, *atom_name;

    memset(buf, 0, sizeof(buf));

    for (;;) {
        if (meta_avail < (int64_t) sizeof(uint32_t)) {
            return 0;
        }

        //size指明了整个box所占用的大小，包括header部分。如果box很大(例如存放具体视频数据的mdat box)，
        // 超过了uint32的最大数值，size就被设置为1，并用接下来的8位uint64来存放大小。
        copied_size = IOBufferReaderCopy(meta_reader, buf, sizeof(mp4_atom_header64));
        atom_size = copied_size > 0 ? mp4_get_32value(buf) : 0;

        if (atom_size == 0) {//如果size 大小为0 说明是最后一个box 直接结束
            return 1;
        }

        atom_header = buf;

        if (atom_size < (int64_t) sizeof(mp4_atom_header)) {//判断是否满足一个32位头大小，如果小于的话，一定是64位的
            if (atom_size == 1) {//如果size 为1 说明是64位的
                if (meta_avail < (int64_t) sizeof(mp4_atom_header64)) { //如果总数据小于64 box header 大小，就再次等待
                    return 0;
                }

            } else {//如果不满足，说明解释失败，直接返回error
                return -1;
            }

            atom_size = mp4_get_64value(atom_header + 8);
            atom_header_size = sizeof(mp4_atom_header64);

        } else { // regular atom

            if (meta_avail < (int64_t) sizeof(mp4_atom_header)) { // not enough for atom header  再次等待数据过来
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
                ret = (this->*mp4_atoms[i].handler)(atom_header_size, atom_size -
                                                                      atom_header_size); // -1: error, 0: unfinished, 1: success

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
Mp4Meta::mp4_atom_next(int64_t atom_size, bool wait) {
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
Mp4Meta::mp4_read_atom(mp4_atom_handler *atom, int64_t size) {
    int i, ret, rc;
    int64_t atom_size, atom_header_size, copied_size;
    char buf[32];
    char *atom_header, *atom_name;

    if (meta_avail < size) { // data insufficient, not reasonable for internal atom box. 数据应该是全的
        return -1;
    }

    while (size > 0) {
        if (meta_avail < (int64_t) sizeof(uint32_t)) { // data insufficient, not reasonable for internal atom box.
            return -1;
        }

        copied_size = IOBufferReaderCopy(meta_reader, buf, sizeof(mp4_atom_header64));
        atom_size = copied_size > 0 ? mp4_get_32value(buf) : 0;

        if (atom_size == 0) {
            return 1;
        }

        atom_header = buf;

        if (atom_size < (int64_t) sizeof(mp4_atom_header)) { //判断是32位还是64位的
            if (atom_size == 1) {
                if (meta_avail < (int64_t) sizeof(mp4_atom_header64)) {
                    return -1;
                }

            } else {
                return -1;
            }

            atom_size = mp4_get_64value(atom_header + 8);
            atom_header_size = sizeof(mp4_atom_header64);

        } else { // regular atom

            if (meta_avail < (int64_t) sizeof(mp4_atom_header)) {
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

                ret = (this->*atom[i].handler)(atom_header_size,
                                               atom_size - atom_header_size); // -1: error, 0: success.

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
Mp4Meta::mp4_read_ftyp_atom(int64_t atom_header_size, int64_t atom_data_size) {
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
    ftyp_size = atom_size;

    return 1;
}

int//读取moov
Mp4Meta::mp4_read_moov_atom(int64_t atom_header_size, int64_t atom_data_size) {
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
Mp4Meta::mp4_read_mvhd_atom(int64_t atom_header_size, int64_t atom_data_size) {
    int64_t atom_size;
    uint32_t timescale;
    mp4_mvhd_atom *mvhd;
    mp4_mvhd64_atom mvhd64;
    uint64_t duration, start_time, length_time;

    if (sizeof(mp4_mvhd_atom) - 8 > (size_t) atom_data_size) {
        return -1;
    }

    memset(&mvhd64, 0, sizeof(mvhd64));
    IOBufferReaderCopy(meta_reader, &mvhd64, sizeof(mp4_mvhd64_atom));
    mvhd = (mp4_mvhd_atom *) &mvhd64;

    if (mvhd->version[0] == 0) {
        timescale = mp4_get_32value(mvhd->timescale);
        duration = mp4_get_32value(mvhd->duration);

    } else { // 64-bit duration
        timescale = mp4_get_32value(mvhd64.timescale);
        duration = mp4_get_64value(mvhd64.duration);
    }

    this->timescale = timescale;  //获取整部电影的time scale
    TSDebug(PLUGIN_NAME, "[mp4_read_mvhd_atom] timescale = %lu, duration=%llu", this->timescale, duration);
    atom_size = atom_header_size + atom_data_size;

    mvhd_atom.buffer = TSIOBufferCreate();
    mvhd_atom.reader = TSIOBufferReaderAlloc(mvhd_atom.buffer);

    TSIOBufferCopy(mvhd_atom.buffer, meta_reader, atom_size, 0);
    mp4_meta_consume(atom_size);


    TSDebug(PLUGIN_NAME, "[mp4_read_mvhd_atom] mvhd timescale:%uD, duration:%uL, time:%.3fs",
            timescale, duration, (double) duration / timescale);

    start_time = (uint64_t) this->start * timescale / 1000;

    if (duration < start_time) {
        TSDebug(PLUGIN_NAME, "[mp4_read_mvhd_atom]  mp4 start time exceeds file duration");
        return -1;
    }

    duration -= start_time;

    if (this->length) {
        length_time = (uint64_t) this->length * timescale / 1000;

        if (duration > length_time) {
            duration = length_time;
        }
    }

    TSDebug(PLUGIN_NAME, "[mp4_read_mvhd_atom] mvhd new duration:%uL, time:%.3fs",
            duration, (double) duration / timescale);

    if (mvhd->version[0] == 0) {

        mp4_reader_set_32value(mvhd_atom.reader, offsetof(mp4_mvhd_atom, duration), duration);
        TSDebug(PLUGIN_NAME, "[mp4_read_mvhd_atom] duration=%llu", duration);

    } else { // 64-bit duration
        mp4_reader_set_64value(mvhd_atom.reader, offsetof(mp4_mvhd64_atom, duration), duration);
        TSDebug(PLUGIN_NAME, "[mp4_read_mvhd_atom] duration=%llu", duration);
    }


    return 1;
}

int//读取track
Mp4Meta::mp4_read_trak_atom(int64_t atom_header_size, int64_t atom_data_size) {
    int rc;
    Mp4Trak *trak;

    if (trak_num >= MP4_MAX_TRAK_NUM - 1) {
        return -1;
    }

    trak = new Mp4Trak();
    trak_vec[trak_num++] = trak;
    TSDebug(PLUGIN_NAME, "[mp4_read_trak_atom] trak_num = %lu", trak_num - 1);

    trak->atoms[MP4_TRAK_ATOM].buffer = TSIOBufferCreate();
    trak->atoms[MP4_TRAK_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_TRAK_ATOM].buffer);

    TSIOBufferCopy(trak->atoms[MP4_TRAK_ATOM].buffer, meta_reader, atom_header_size, 0);// box header
    mp4_meta_consume(atom_header_size);

    rc = mp4_read_atom(mp4_trak_atoms, atom_data_size);//读取tkhd + media

    return rc;
}

int Mp4Meta::mp4_read_cmov_atom(int64_t /*atom_header_size ATS_UNUSED */, int64_t /* atom_data_size ATS_UNUSED */) {
    return -1;
}

int
Mp4Meta::mp4_read_tkhd_atom(int64_t atom_header_size, int64_t atom_data_size) {
    int64_t atom_size, need;
    Mp4Trak *trak;
    uint64_t duration, start_time, length_time;

    atom_size = atom_header_size + atom_data_size;

    trak = trak_vec[trak_num - 1];
    trak->tkhd_size = atom_size;//track header box size

    trak->atoms[MP4_TKHD_ATOM].buffer = TSIOBufferCreate();
    trak->atoms[MP4_TKHD_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_TKHD_ATOM].buffer);

    TSIOBufferCopy(trak->atoms[MP4_TKHD_ATOM].buffer, meta_reader, atom_size, 0);
    mp4_meta_consume(atom_size);

    mp4_reader_set_32value(trak->atoms[MP4_TKHD_ATOM].reader, offsetof(mp4_tkhd_atom, size), atom_size);//设置一下tkhd 的总大小


    mp4_tkhd_atom *tkhd_atom;
    mp4_tkhd64_atom tkhd64_atom;

    need = TSIOBufferReaderAvail(trak->atoms[MP4_TKHD_ATOM].reader);

    if (need > (int64_t) sizeof(mp4_tkhd64_atom)) {
        need = sizeof(mp4_tkhd64_atom);
    }

    memset(&tkhd64_atom, 0, sizeof(tkhd64_atom));
    IOBufferReaderCopy(trak->atoms[MP4_TKHD_ATOM].reader, &tkhd64_atom, need);
    tkhd_atom = (mp4_tkhd_atom *) &tkhd64_atom;

    if (tkhd_atom->version[0] == 0) {
        duration = mp4_get_32value(tkhd_atom->duration);
        TSDebug(PLUGIN_NAME, "[mp4_read_tkhd_atom] duration=%llu", duration);

    } else {
        duration = mp4_get_64value(tkhd64_atom.duration);
        TSDebug(PLUGIN_NAME, "[mp4_read_tkhd_atom] duration=%llu", duration);

    }

    start_time = (uint64_t) this->start * this->timescale / 1000;
    if (duration <= start_time) {
        TSDebug(PLUGIN_NAME, "[mp4_read_tkhd_atom] tkhd duration is less than start time");
        return -1;
    }

    duration -= start_time;

    if (this->length) {
        length_time = (uint64_t) this->length * this->timescale / 1000;

        if (duration > length_time) {
            duration = length_time;
        }
    }

    TSDebug(PLUGIN_NAME, "[mp4_read_tkhd_atom] tkhd new duration:%uL, time:%.3fs", duration,
            (double) duration / this->timescale);

    if (tkhd_atom->version[0] == 0) {
        mp4_reader_set_32value(trak->atoms[MP4_TKHD_ATOM].reader, offsetof(mp4_tkhd_atom, duration), duration);
        TSDebug(PLUGIN_NAME, "[mp4_read_tkhd_atom] duration=%llu", duration);

    } else {
        mp4_reader_set_64value(trak->atoms[MP4_TKHD_ATOM].reader, offsetof(mp4_tkhd64_atom, duration), duration);
        TSDebug(PLUGIN_NAME, "[mp4_read_tkhd_atom] duration=%llu", duration);

    }

    return 1;
}

int
Mp4Meta::mp4_read_mdia_atom(int64_t atom_header_size, int64_t atom_data_size) {
    Mp4Trak *trak;

    trak = trak_vec[trak_num - 1];

    trak->atoms[MP4_MDIA_ATOM].buffer = TSIOBufferCreate();
    trak->atoms[MP4_MDIA_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_MDIA_ATOM].buffer);

    TSIOBufferCopy(trak->atoms[MP4_MDIA_ATOM].buffer, meta_reader, atom_header_size, 0);//读取 box header
    mp4_meta_consume(atom_header_size);

    return mp4_read_atom(mp4_mdia_atoms, atom_data_size);
}

int
Mp4Meta::mp4_read_mdhd_atom(int64_t atom_header_size, int64_t atom_data_size) {
    int64_t atom_size;
    uint64_t duration, start_time, length_time;
    uint32_t ts;
    Mp4Trak *trak;
    mp4_mdhd_atom *mdhd;
    mp4_mdhd64_atom mdhd64;

    memset(&mdhd64, 0, sizeof(mdhd64));
    IOBufferReaderCopy(meta_reader, &mdhd64, sizeof(mp4_mdhd64_atom));
    mdhd = (mp4_mdhd_atom *) &mdhd64;

    if (mdhd->version[0] == 0) {
        ts = mp4_get_32value(mdhd->timescale);
        duration = mp4_get_32value(mdhd->duration);

    } else {
        ts = mp4_get_32value(mdhd64.timescale);
        duration = mp4_get_64value(mdhd64.duration);
    }

    atom_size = atom_header_size + atom_data_size;

    trak = trak_vec[trak_num - 1];
    trak->mdhd_size = atom_size;
    //时长 = duration / timescale
    trak->timescale = ts;
//    trak->duration = duration;

    trak->atoms[MP4_MDHD_ATOM].buffer = TSIOBufferCreate();
    trak->atoms[MP4_MDHD_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_MDHD_ATOM].buffer);

    TSIOBufferCopy(trak->atoms[MP4_MDHD_ATOM].buffer, meta_reader, atom_size, 0);
    mp4_meta_consume(atom_size);

    mp4_reader_set_32value(trak->atoms[MP4_MDHD_ATOM].reader, offsetof(mp4_mdhd_atom, size), atom_size);//重新设置大小

    start_time = (uint64_t) this->start * ts / 1000;
    if (duration <= start_time) {
        TSDebug(PLUGIN_NAME, "[mp4_read_mdhd_atom] mdhd duration is less than start time");
        return -1;
    }

    duration -= start_time;

    if (this->length) {
        length_time = (uint64_t) this->length * ts / 1000;

        if (duration > length_time) {
            duration = length_time;
        }
    }
    TSDebug(PLUGIN_NAME, "[mp4_read_mdhd_atom] mdhd new duration:%uL, time:%.3fs", duration, (double) duration / ts);

    trak->duration = duration;

    if (mdhd->version[0] == 0) {
        mp4_reader_set_32value(trak->atoms[MP4_MDHD_ATOM].reader, offsetof(mp4_mdhd_atom, duration), duration);
        TSDebug(PLUGIN_NAME, "[mp4_update_mdhd_duration] duration=%llu", duration);
    } else {
        mp4_reader_set_64value(trak->atoms[MP4_MDHD_ATOM].reader, offsetof(mp4_mdhd64_atom, duration), duration);
        TSDebug(PLUGIN_NAME, "[mp4_update_mdhd_duration] duration=%llu", duration);

    }

    return 1;
}

int
Mp4Meta::mp4_read_hdlr_atom(int64_t atom_header_size, int64_t atom_data_size) {
    int64_t atom_size;
    Mp4Trak *trak;

    atom_size = atom_header_size + atom_data_size;

    trak = trak_vec[trak_num - 1];
    trak->hdlr_size = atom_size;

    trak->atoms[MP4_HDLR_ATOM].buffer = TSIOBufferCreate();
    trak->atoms[MP4_HDLR_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_HDLR_ATOM].buffer);

    TSIOBufferCopy(trak->atoms[MP4_HDLR_ATOM].buffer, meta_reader, atom_size, 0);
    mp4_meta_consume(atom_size);

    return 1;
}

int
Mp4Meta::mp4_read_minf_atom(int64_t atom_header_size, int64_t atom_data_size) {
    Mp4Trak *trak;

    trak = trak_vec[trak_num - 1];

    trak->atoms[MP4_MINF_ATOM].buffer = TSIOBufferCreate();
    trak->atoms[MP4_MINF_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_MINF_ATOM].buffer);

    TSIOBufferCopy(trak->atoms[MP4_MINF_ATOM].buffer, meta_reader, atom_header_size, 0);
    mp4_meta_consume(atom_header_size);

    return mp4_read_atom(mp4_minf_atoms, atom_data_size);
}

int
Mp4Meta::mp4_read_vmhd_atom(int64_t atom_header_size, int64_t atom_data_size) {
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
Mp4Meta::mp4_read_smhd_atom(int64_t atom_header_size, int64_t atom_data_size) {
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
Mp4Meta::mp4_read_dinf_atom(int64_t atom_header_size, int64_t atom_data_size) {
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
Mp4Meta::mp4_read_stbl_atom(int64_t atom_header_size, int64_t atom_data_size) {
    Mp4Trak *trak;

    trak = trak_vec[trak_num - 1];

    trak->atoms[MP4_STBL_ATOM].buffer = TSIOBufferCreate();
    trak->atoms[MP4_STBL_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STBL_ATOM].buffer);

    TSIOBufferCopy(trak->atoms[MP4_STBL_ATOM].buffer, meta_reader, atom_header_size, 0);
    mp4_meta_consume(atom_header_size);

    return mp4_read_atom(mp4_stbl_atoms, atom_data_size);
}

int//sample description box
Mp4Meta::mp4_read_stsd_atom(int64_t atom_header_size, int64_t atom_data_size) {
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
Mp4Meta::mp4_read_stts_atom(int64_t atom_header_size, int64_t atom_data_size) {
    int32_t entries;
    int64_t esize, copied_size;
    mp4_stts_atom stts;
    Mp4Trak *trak;


    if (sizeof(mp4_stts_atom) - 8 > (size_t) atom_data_size) {
        return -1;
    }

    copied_size = IOBufferReaderCopy(meta_reader, &stts, sizeof(mp4_stts_atom));
    entries = copied_size > 0 ? mp4_get_32value(stts.entries) : 0;
    esize = entries * sizeof(mp4_stts_entry);

    if (sizeof(mp4_stts_atom) - 8 + esize > (size_t) atom_data_size) {
        return -1;
    }

    trak = trak_vec[trak_num - 1];
    trak->time_to_sample_entries = entries;

    trak->stts_pos = 0;
    trak->stts_last = (uint32_t) entries;

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
Mp4Meta::mp4_read_stss_atom(int64_t atom_header_size, int64_t atom_data_size) {
    int32_t entries;
    int64_t esize, copied_size;
    mp4_stss_atom stss;
    Mp4Trak *trak;

    if (sizeof(mp4_stss_atom) - 8 > (size_t) atom_data_size) {
        return -1;
    }

    copied_size = IOBufferReaderCopy(meta_reader, &stss, sizeof(mp4_stss_atom));
    entries = copied_size > 0 ? mp4_get_32value(stss.entries) : 0;
    esize = entries * sizeof(int32_t); //sample id

    if (sizeof(mp4_stss_atom) - 8 + esize > (size_t) atom_data_size) {
        return -1;
    }

    trak = trak_vec[trak_num - 1];
    trak->sync_samples_entries = entries;

    trak->stss_pos = 0;
    trak->stss_last = entries;

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
Mp4Meta::mp4_read_ctts_atom(int64_t atom_header_size, int64_t atom_data_size) {
    int32_t entries;
    int64_t esize, copied_size;
    mp4_ctts_atom ctts;
    Mp4Trak *trak;

    if (sizeof(mp4_ctts_atom) - 8 > (size_t) atom_data_size) {
        return -1;
    }

    copied_size = IOBufferReaderCopy(meta_reader, &ctts, sizeof(mp4_ctts_atom));
    entries = copied_size > 0 ? mp4_get_32value(ctts.entries) : 0;
    esize = entries * sizeof(mp4_ctts_entry);

    if (sizeof(mp4_ctts_atom) - 8 + esize > (size_t) atom_data_size) {
        return -1;
    }

    trak = trak_vec[trak_num - 1];
    trak->composition_offset_entries = entries;

    trak->ctts_pos = 0;
    trak->ctts_last = entries;

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
Mp4Meta::mp4_read_stsc_atom(int64_t atom_header_size, int64_t atom_data_size) {
    int32_t entries;
    int64_t esize, copied_size;
    mp4_stsc_atom stsc;
    Mp4Trak *trak;

    if (sizeof(mp4_stsc_atom) - 8 > (size_t) atom_data_size) {
        return -1;
    }

    copied_size = IOBufferReaderCopy(meta_reader, &stsc, sizeof(mp4_stsc_atom));
    entries = copied_size > 0 ? mp4_get_32value(stsc.entries) : 0;
    esize = entries * sizeof(mp4_stsc_entry);

    if (sizeof(mp4_stsc_atom) - 8 + esize > (size_t) atom_data_size) {
        return -1;
    }

    trak = trak_vec[trak_num - 1];
    trak->sample_to_chunk_entries = entries;

    trak->stsc_pos = 0;
    trak->stsc_last = entries;

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
Mp4Meta::mp4_read_stsz_atom(int64_t atom_header_size, int64_t atom_data_size) {
    int32_t entries, size;
    int64_t esize, atom_size, copied_size;
    mp4_stsz_atom stsz;
    Mp4Trak *trak;

    if (sizeof(mp4_stsz_atom) - 8 > (size_t) atom_data_size) {
        return -1;
    }

    copied_size = IOBufferReaderCopy(meta_reader, &stsz, sizeof(mp4_stsz_atom));
    entries = copied_size > 0 ? mp4_get_32value(stsz.entries) : 0;
    esize = entries * sizeof(int32_t); //sample size

    trak = trak_vec[trak_num - 1];
    size = copied_size > 0 ? mp4_get_32value(stsz.uniform_size) : 0;

    trak->sample_sizes_entries = entries;

    trak->stsz_pos = 0;
    trak->stsz_last = entries;

    trak->atoms[MP4_STSZ_ATOM].buffer = TSIOBufferCreate();
    trak->atoms[MP4_STSZ_ATOM].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STSZ_ATOM].buffer);
    TSIOBufferCopy(trak->atoms[MP4_STSZ_ATOM].buffer, meta_reader, sizeof(mp4_stsz_atom), 0);

    if (size == 0) {//全部sample 数目，如果所有的sample有相同的长度，这个字段就是这个值，否则就是0
        if (sizeof(mp4_stsz_atom) - 8 + esize > (size_t) atom_data_size) {
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
Mp4Meta::mp4_read_stco_atom(int64_t atom_header_size, int64_t atom_data_size) {
    int32_t entries;
    int64_t esize, copied_size;
    mp4_stco_atom stco;
    Mp4Trak *trak;

    if (sizeof(mp4_stco_atom) - 8 > (size_t) atom_data_size) {
        return -1;
    }

    copied_size = IOBufferReaderCopy(meta_reader, &stco, sizeof(mp4_stco_atom));
    entries = copied_size > 0 ? mp4_get_32value(stco.entries) : 0;
    esize = entries * sizeof(int32_t);

    if (sizeof(mp4_stco_atom) - 8 + esize > (size_t) atom_data_size) {
        return -1;
    }

    trak = trak_vec[trak_num - 1];
    trak->chunks = entries;
    // entries = 16391,trak_num=0
    TSDebug(PLUGIN_NAME, "[mp4_read_stco_atom] entries = %d,trak_num=%lu", entries, trak_num - 1);
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
Mp4Meta::mp4_read_co64_atom(int64_t atom_header_size, int64_t atom_data_size) {
    int32_t entries;
    int64_t esize, copied_size;
    mp4_co64_atom co64;
    Mp4Trak *trak;

    if (sizeof(mp4_co64_atom) - 8 > (size_t) atom_data_size) {
        return -1;
    }

    copied_size = IOBufferReaderCopy(meta_reader, &co64, sizeof(mp4_co64_atom));
    entries = copied_size > 0 ? mp4_get_32value(co64.entries) : 0;
    esize = entries * sizeof(int64_t);

    if (sizeof(mp4_co64_atom) - 8 + esize > (size_t) atom_data_size) {
        return -1;
    }

    trak = trak_vec[trak_num - 1];
    trak->chunks = entries;
    TSDebug(PLUGIN_NAME, "[mp4_read_co64_atom] entries = %d,trak_num=%lu", entries, trak_num - 1);
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
int Mp4Meta::mp4_read_mdat_atom(int64_t /* atom_header_size ATS_UNUSED */, int64_t /* atom_data_size ATS_UNUSED */) {
    mdat_atom.buffer = TSIOBufferCreate();
    mdat_atom.reader = TSIOBufferReaderAlloc(mdat_atom.buffer);

    meta_complete = true;
    return 1;
}

int
Mp4Meta::mp4_crop_stts_data(Mp4Trak *trak, uint start) {

    uint32_t count, duration, rest;
    uint64_t start_time;
    TSIOBufferReader readerp;
    uint32_t start_sample, entries, start_sec;
    uint32_t entry, end;

    if (start) {
        start_sec = this->start;

        TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data] mp4 stts crop start_time:%ui", start_sec);

    } else if (this->length) {
        start_sec = this->length;

        TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data] mp4 stts crop end_time:%ui", start_sec);

    } else {
        return 0;
    }

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STTS_DATA].reader);

    start_time = (uint64_t) start_sec * trak->timescale / 1000;

    entries = trak->time_to_sample_entries;
    start_sample = 0;

    entry = trak->stts_pos;
    end = trak->stts_last;

    while (entry < end) {//根据时间查找
        duration = (uint32_t) mp4_reader_get_32value(readerp, offsetof(mp4_stts_entry, duration));
        count = (uint32_t) mp4_reader_get_32value(readerp, offsetof(mp4_stts_entry, count));
        //mp4_update_stts_atom duration = 3200, count = 16392
        TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_start] time:%uL, count:%uD, duration:%uD",
                start_time, count, duration);
        if (start_time < (uint64_t) count * duration) {
            start_sample = (uint32_t) (start_time / duration);
            rest = (uint32_t) (start_time / duration);
            goto found;
        }

        start_sample += count;//计算已经丢弃了多少个sample
        start_time -= (uint64_t) count * duration; //还剩多少时间
        entries--;
        entry++;
        TSIOBufferReaderConsume(readerp, sizeof(mp4_stts_entry)); //丢弃
    }

    TSIOBufferReaderFree(readerp);
    if (start) {
        TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_start] start time is out mp4 stts samples");

        return -1;

    } else {
        trak->end_sample = trak->start_sample + start_sample;

        TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_start] end_sample:%ui", trak->end_sample);

        return 0;
    }

    found:

    if (start) {
        mp4_reader_set_32value(readerp, offsetof(mp4_stts_entry, count), count - rest);
        trak->stts_pos = entry;
        trak->time_to_sample_entries = entries;
        trak->start_sample = start_sample;

        TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_start] start_sample:%ui, new count:%uD",
                trak->start_sample, count - rest);

    } else {
        mp4_reader_set_32value(readerp, offsetof(mp4_stts_entry, count), rest);
        trak->stts_last = entry + 1;
        trak->time_to_sample_entries -= entries - 1;
        trak->end_sample = trak->start_sample + start_sample;

        TSDebug(PLUGIN_NAME, "[mp4_crop_stts_data_start] end_sample:%ui, new count:%uD",
                trak->end_sample, rest);
    }
    TSIOBufferReaderFree(readerp);
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
Mp4Meta::mp4_update_stts_atom(Mp4Trak *trak) {

    size_t atom_size;
    int64_t avail, copy_avail, new_avail;

    /*
     * mdia.minf.stbl.stts updating requires trak->timescale
     * from mdia.mdhd atom which may reside after mdia.minf
     */

    TSDebug(PLUGIN_NAME, "[mp4_update_stts_atom] mp4 stts atom update");

    if (trak->atoms[MP4_STTS_DATA].buffer == nullptr) {
        TSDebug(PLUGIN_NAME, "[mp4_update_stts_atom] no mp4 stts atoms were found");
        return -1;
    }

    if (mp4_crop_stts_data(trak, 1) < 0) {
        return -1;
    }

    if (mp4_crop_stts_data(trak, 0) < 0) {
        return -1;
    }

    TSDebug(PLUGIN_NAME, "[mp4_update_stts_atom] time-to-sample entries:%uD", trak->time_to_sample_entries);

    atom_size = sizeof(mp4_stts_atom) + (trak->stts_last - trak->stts_pos) * sizeof(mp4_stts_entry);

    TSDebug(PLUGIN_NAME, "[mp4_update_stts_atom] sizeof(mp4_stts_atom) =%lu,atom_size=%llu",sizeof(mp4_stts_atom), atom_size);

    trak->size += atom_size;

    TSIOBufferReaderConsume(trak->atoms[MP4_STTS_DATA].reader, trak->stts_pos * sizeof(mp4_stts_entry));

    new_avail = (trak->stts_last - trak->stts_pos) * sizeof(mp4_stts_entry);
    copy_avail = TSIOBufferReaderAvail(copy_reader);
    if (copy_avail > 0) {
        TSIOBufferReaderConsume(copy_reader, copy_avail);
    }
    avail = TSIOBufferReaderAvail(trak->atoms[MP4_STTS_DATA].reader);
    if (new_avail > avail)
        new_avail = avail;
    TSDebug(PLUGIN_NAME, "[mp4_update_stts_atom] avail=%lld, new_avail=%lld", avail, new_avail);
    TSIOBufferCopy(copy_buffer, trak->atoms[MP4_STTS_DATA].reader, new_avail, 0);
    TSIOBufferReaderConsume(trak->atoms[MP4_STTS_DATA].reader, avail);

    TSIOBufferCopy(trak->atoms[MP4_STTS_DATA].buffer, copy_reader, new_avail, 0);


    mp4_reader_set_32value(trak->atoms[MP4_STTS_ATOM].reader, offsetof(mp4_stts_atom, size), atom_size);
    mp4_reader_set_32value(trak->atoms[MP4_STTS_ATOM].reader, offsetof(mp4_stts_atom, entries),
                           trak->time_to_sample_entries);

    return 0;
}


int
Mp4Meta::mp4_crop_stss_data(Mp4Trak *trak, uint start) {
    uint32_t sample, start_sample, entry, end;
    uint32_t entries;
    TSIOBufferReader readerp;

    /* sync samples starts from 1 */

    if (start) {
        start_sample = trak->start_sample + 1;

        TSDebug(PLUGIN_NAME, "[mp4_crop_stss_data] mp4 stss crop start_sample:%uD", start_sample);

    } else if (this->length) {
        start_sample = trak->end_sample + 1;

        TSDebug(PLUGIN_NAME, "[mp4_crop_stss_data] mp4 stss crop end_sample:%uD", start_sample);

    } else {
        return 0;
    }

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSS_DATA].reader);

    entries = trak->sync_samples_entries;
    entry = trak->stss_pos;
    end = trak->stss_last;

    while (entry < end) {
        sample = (uint32_t) mp4_reader_get_32value(readerp, 0);

        TSDebug(PLUGIN_NAME, "[mp4_crop_stss_data] sync:%uD", sample);

        if (sample >= start_sample) {
            goto found;
        }

        entries--;
        entry++;
        TSIOBufferReaderConsume(readerp, sizeof(uint32_t));
    }

    TSDebug(PLUGIN_NAME, "[mp4_crop_stss_data] sample is out of mp4 stss atom");

    found:

    if (start) {
        trak->stss_pos = entry;
        trak->sync_samples_entries = entries;

    } else {
        trak->stss_last = entry;
        trak->sync_samples_entries -= entries;
    }
    TSIOBufferReaderFree(readerp);
    return 0;
}

/**
 * Sync Sample Box
 * size, type, version flags, number of entries
 *
 * 该box 决定了整个mp4 文件是否可以拖动，如果box 只有一个entry,则拖拉时，进度到最后
 */
int
Mp4Meta::mp4_update_stss_atom(Mp4Trak *trak) {
    size_t atom_size;
    uint32_t sample, start_sample, entry, end;
    TSIOBufferReader readerp;
    int64_t avail, copy_avail, new_avail;

    /*
     * mdia.minf.stbl.stss updating requires trak->start_sample
     * from mdia.minf.stbl.stts which depends on value from mdia.mdhd
     * atom which may reside after mdia.minf
     */

    TSDebug(PLUGIN_NAME, "[mp4_update_stss_atom] mp4 stss atom update");

    if (trak->atoms[MP4_STSS_DATA].buffer == nullptr) {
        return 0;
    }

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSS_DATA].reader);

    mp4_crop_stss_data(trak, 1);
    mp4_crop_stss_data(trak, 0);

    TSDebug(PLUGIN_NAME, "[mp4_update_stss_atom] sync sample entries:%uD", trak->sync_samples_entries);

    if (trak->sync_samples_entries) {
        entry = trak->stss_pos;
        end = trak->stss_last;

        start_sample = trak->start_sample;

        while (entry < end) {
            sample = (uint32_t) mp4_reader_get_32value(readerp, 0);
            sample -= start_sample;
            mp4_reader_set_32value(readerp, 0, sample);
            TSIOBufferReaderConsume(readerp, sizeof(uint32_t));
            entry++;
        }

    } else {
        TSIOBufferReaderFree(trak->atoms[MP4_STSS_DATA].reader);
        TSIOBufferDestroy(trak->atoms[MP4_STSS_DATA].buffer);

        trak->atoms[MP4_STSS_DATA].reader = nullptr;
        trak->atoms[MP4_STSS_DATA].buffer = nullptr;
    }

    atom_size = sizeof(mp4_stss_atom) + (trak->stss_last - trak->stss_pos) * sizeof(uint32_t);

    TSDebug(PLUGIN_NAME, "[mp4_update_stss_atom] sizeof(mp4_stss_atom) =%lu,atom_size=%llu",sizeof(mp4_stss_atom), atom_size);

    trak->size += atom_size;

    TSIOBufferReaderConsume(trak->atoms[MP4_STSS_DATA].reader, trak->stss_pos * sizeof(uint32_t));


    new_avail = (trak->stss_last - trak->stss_pos) * sizeof(uint32_t);
    copy_avail = TSIOBufferReaderAvail(copy_reader);
    if (copy_avail > 0) {
        TSIOBufferReaderConsume(copy_reader, copy_avail);
    }
    avail = TSIOBufferReaderAvail(trak->atoms[MP4_STSS_DATA].reader);
    if (new_avail > avail)
        new_avail = avail;
    TSDebug(PLUGIN_NAME, "[mp4_update_stss_atom] avail=%lld, new_avail=%lld", avail, new_avail);
    TSIOBufferCopy(copy_buffer, trak->atoms[MP4_STSS_DATA].reader, new_avail, 0);
    TSIOBufferReaderConsume(trak->atoms[MP4_STSS_DATA].reader, avail);

    TSIOBufferCopy(trak->atoms[MP4_STSS_DATA].buffer, copy_reader, new_avail, 0);


    mp4_reader_set_32value(trak->atoms[MP4_STSS_ATOM].reader, offsetof(mp4_stss_atom, size), atom_size);
    mp4_reader_set_32value(trak->atoms[MP4_STSS_ATOM].reader, offsetof(mp4_stss_atom, entries),
                           trak->sync_samples_entries);

    return 0;
}


int
Mp4Meta::mp4_crop_ctts_data(Mp4Trak *trak, uint start) {
    uint32_t count, start_sample, rest;
    uint32_t entries;
    uint32_t entry, end;
    TSIOBufferReader readerp;

    /* sync samples starts from 1 */

    if (start) {
        start_sample = trak->start_sample + 1;

        TSDebug(PLUGIN_NAME, "[mp4_crop_ctts_data] mp4 ctts crop start_sample:%uD", start_sample);

    } else if (this->length) {
        start_sample = trak->end_sample - trak->start_sample + 1;

        TSDebug(PLUGIN_NAME, "[mp4_crop_ctts_data] mp4 ctts crop end_sample:%uD", start_sample);

    } else {
        return 0;
    }

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_CTTS_DATA].reader);

    entries = trak->composition_offset_entries;
    entry = trak->ctts_pos;
    end = trak->ctts_last;

    while (entry < end) {
        count = (uint32_t) mp4_reader_get_32value(readerp, offsetof(mp4_ctts_entry, count));

//        TSDebug(PLUGIN_NAME, "[mp4_crop_ctts_data] sample:%uD, count:%uD, offset:%uD",
//                start_sample, count, mp4_reader_get_32value(readerp, offsetof(mp4_ctts_entry, offset)));

        if (start_sample <= count) {
            rest = start_sample - 1;
            goto found;
        }

        start_sample -= count;
        entries--;
        entry++;
        TSIOBufferReaderConsume(readerp, sizeof(mp4_ctts_entry));
    }

    if (start) {
        trak->ctts_pos = end;
        trak->composition_offset_entries = 0;
    }

    TSIOBufferReaderFree(readerp);
    return 0;

    found:

    if (start) {
        mp4_reader_set_32value(readerp, offsetof(mp4_ctts_entry, count), count - rest);
        trak->ctts_pos = entry;
        trak->composition_offset_entries = entries;

    } else {
        mp4_reader_set_32value(readerp, offsetof(mp4_ctts_entry, count), rest);
        trak->ctts_last = (entry + 1);
        trak->composition_offset_entries -= entries - 1;
    }

    TSIOBufferReaderFree(readerp);
    return 0;
}

int
Mp4Meta::mp4_update_ctts_atom(Mp4Trak *trak) {

    size_t atom_size;
    int64_t avail, copy_avail, new_avail;

    /*
     * mdia.minf.stbl.ctts updating requires trak->start_sample
     * from mdia.minf.stbl.stts which depends on value from mdia.mdhd
     * atom which may reside after mdia.minf
     */

    TSDebug(PLUGIN_NAME, "[mp4_update_ctts_atom] mp4 ctts atom update");

    if (trak->atoms[MP4_CTTS_DATA].buffer == nullptr) {
        return 0;
    }

    mp4_crop_ctts_data(trak, 1);
    mp4_crop_ctts_data(trak, 0);

    TSDebug(PLUGIN_NAME, "[mp4_update_ctts_atom] composition offset entries:%uD",
            trak->composition_offset_entries);

    if (trak->composition_offset_entries == 0) {
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
        return 0;
    }

    atom_size = sizeof(mp4_ctts_atom) + (trak->ctts_last - trak->ctts_pos) * sizeof(mp4_ctts_entry);

    TSDebug(PLUGIN_NAME, "[mp4_update_ctts_atom] sizeof(mp4_ctts_atom) =%lu,atom_size=%llu",sizeof(mp4_ctts_atom), atom_size);

    trak->size += atom_size;

    TSIOBufferReaderConsume(trak->atoms[MP4_CTTS_DATA].reader, trak->ctts_pos * sizeof(mp4_ctts_entry));

    new_avail = (trak->ctts_last - trak->ctts_pos) * sizeof(mp4_ctts_entry);
    copy_avail = TSIOBufferReaderAvail(copy_reader);
    if (copy_avail > 0) {
        TSIOBufferReaderConsume(copy_reader, copy_avail);
    }
    avail = TSIOBufferReaderAvail(trak->atoms[MP4_CTTS_DATA].reader);
    if (new_avail > avail)
        new_avail = avail;
    TSDebug(PLUGIN_NAME, "[mp4_update_ctts_atom] avail=%lld, new_avail=%lld", avail, new_avail);
    TSIOBufferCopy(copy_buffer, trak->atoms[MP4_CTTS_DATA].reader, new_avail, 0);
    TSIOBufferReaderConsume(trak->atoms[MP4_CTTS_DATA].reader, avail);

    TSIOBufferCopy(trak->atoms[MP4_CTTS_DATA].buffer, copy_reader, new_avail, 0);


    mp4_reader_set_32value(trak->atoms[MP4_CTTS_ATOM].reader, offsetof(mp4_ctts_atom, size), atom_size);
    mp4_reader_set_32value(trak->atoms[MP4_CTTS_ATOM].reader, offsetof(mp4_ctts_atom, entries),
                           trak->composition_offset_entries);

    return 0;
}


int
Mp4Meta::mp4_crop_stsc_data(Mp4Trak *trak, uint start) {

    uint32_t start_sample, chunk, samples, id, next_chunk, n,
            prev_samples;
    uint32_t entries, target_chunk, chunk_samples;
    uint32_t entry, end;
    mp4_stsc_entry *first;
    TSIOBufferReader readerp;

    entries = trak->sample_to_chunk_entries - 1;

    if (start) {
        start_sample = (uint32_t) trak->start_sample;

        TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data] mp4 stsc crop start_sample:%uD", start_sample);

    } else if (this->length) {
        start_sample = (uint32_t) (trak->end_sample - trak->start_sample);
        samples = 0;

        readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSC_DATA].reader);

        if (trak->atoms[MP4_STSC_DATA].buffer != nullptr) {
            samples = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, samples));
            entries--;

            if (samples > start_sample) {
                samples = start_sample;
                mp4_reader_set_32value(readerp, offsetof(mp4_stsc_entry, samples), samples);
            }

            start_sample -= samples;
        }
        TSIOBufferReaderFree(readerp);
        TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data] mp4 stsc crop end_sample:%uD, ext_samples:%uD",
                start_sample, samples);

    } else {
        return 0;
    }

    entry = trak->stsc_pos;
    end = trak->stsc_last;

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSC_DATA].reader);

    chunk = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, chunk));
    samples = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, samples));
    id = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, id));
    TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry));
    prev_samples = 0;
    entry++;

    while (entry < end) {

        next_chunk = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, chunk));

//        TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data] sample:%uD, chunk:%uD, chunks:%uD, "
//                        "samples:%uD, id:%uD",
//                start_sample, chunk, next_chunk - chunk, samples, id);

        n = (next_chunk - chunk) * samples;

        if (start_sample < n) {
            goto found;
        }

        start_sample -= n;

        prev_samples = samples;
        chunk = next_chunk;
        samples = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, samples));
        id = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, id));
        entries--;
        entry++;
        TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry));
    }

    next_chunk = trak->chunks + 1;

    TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data] sample:%uD, chunk:%uD, chunks:%uD, samples:%uD",
            start_sample, chunk, next_chunk - chunk, samples);

    n = (next_chunk - chunk) * samples;

    if (start_sample > n) {
        TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data] %s time is out mp4 stsc chunks",
                start ? "start" : "end");
        TSIOBufferReaderFree(readerp);
        return -1;
    }

    found:
    TSIOBufferReaderFree(readerp);
    entries++;
    entry--;

    if (samples == 0) {
        TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data] zero number of samples");
        return -1;
    }

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSC_DATA].reader);
    TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry) * (entry - 1));

    target_chunk = chunk - 1;
    target_chunk += start_sample / samples;
    chunk_samples = start_sample % samples;

    if (start) {
        trak->stsc_pos = entry;

        trak->sample_to_chunk_entries = entries;
        trak->start_chunk = target_chunk;
        trak->start_chunk_samples = chunk_samples;

        mp4_reader_set_32value(readerp, offsetof(mp4_stsc_entry, chunk), trak->start_chunk + 1);


        samples -= chunk_samples;

        TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data] start_chunk:%ui, start_chunk_samples:%ui",
                trak->start_chunk, trak->start_chunk_samples);

    } else {
        if (start_sample) {
            trak->stsc_last = (entry + 1);
            trak->sample_to_chunk_entries -= entries - 1;
            trak->end_chunk_samples = samples;

        } else {
            trak->stsc_last = entry;
            trak->sample_to_chunk_entries -= entries;
            trak->end_chunk_samples = prev_samples;
        }

        if (chunk_samples) {
            trak->end_chunk = target_chunk + 1;
            trak->end_chunk_samples = chunk_samples;

        } else {
            trak->end_chunk = target_chunk;
        }

        samples = chunk_samples;
        next_chunk = chunk + 1;

        TSDebug(PLUGIN_NAME, "[mp4_crop_stsc_data] end_chunk:%ui, end_chunk_samples:%ui",
                trak->end_chunk, trak->end_chunk_samples);
    }

    if (chunk_samples && next_chunk - target_chunk == 2) {

        mp4_reader_set_32value(readerp, offsetof(mp4_stsc_entry, samples), samples);

    } else if (chunk_samples && start) {

        first = &trak->stsc_chunk_entry;
        mp4_set_32value(first->chunk, 1);
        mp4_set_32value(first->samples, samples);
        mp4_set_32value(first->id, id);

        trak->atoms[MP4_STSC_CHUNK_START].buffer = TSIOBufferSizedCreate(TS_IOBUFFER_SIZE_INDEX_128);
        trak->atoms[MP4_STSC_CHUNK_START].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STSC_CHUNK_START].buffer);
        TSIOBufferWrite(trak->atoms[MP4_STSC_CHUNK_START].buffer, first, sizeof(mp4_stsc_entry));

        mp4_reader_set_32value(readerp, offsetof(mp4_stsc_entry, chunk), 2);


        trak->sample_to_chunk_entries++;

    } else if (chunk_samples) {

        first = &trak->stsc_chunk_entry;
        mp4_set_32value(first->chunk, trak->end_chunk - trak->start_chunk);
        mp4_set_32value(first->samples, samples);
        mp4_set_32value(first->id, id);

        trak->atoms[MP4_STSC_CHUNK_END].buffer = TSIOBufferSizedCreate(TS_IOBUFFER_SIZE_INDEX_128);
        trak->atoms[MP4_STSC_CHUNK_END].reader = TSIOBufferReaderAlloc(trak->atoms[MP4_STSC_CHUNK_END].buffer);
        TSIOBufferWrite(trak->atoms[MP4_STSC_CHUNK_END].buffer, first, sizeof(mp4_stsc_entry));

        trak->sample_to_chunk_entries++;
    }

    TSIOBufferReaderFree(readerp);
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
Mp4Meta::mp4_update_stsc_atom(Mp4Trak *trak) {
    size_t atom_size;
    uint32_t chunk;
    uint32_t entry, end;
    TSIOBufferReader readerp;
    int64_t avail, copy_avail, new_avail;

    /*
     * mdia.minf.stbl.stsc updating requires trak->start_sample
     * from mdia.minf.stbl.stts which depends on value from mdia.mdhd
     * atom which may reside after mdia.minf
     */

    TSDebug(PLUGIN_NAME, "[mp4_update_stsc_atom] mp4 stsc atom update");

    if (trak->atoms[MP4_STSC_DATA].buffer == nullptr) {
        TSDebug(PLUGIN_NAME, "[mp4_update_stsc_atom] no mp4 stsc atoms were found");
        return -1;
    }

    if (trak->sample_to_chunk_entries == 0) {
        TSDebug(PLUGIN_NAME, "[mp4_update_stsc_atom] zero number of entries in stsc atom");
        return -1;
    }

    if (mp4_crop_stsc_data(trak, 1) < 0) {
        return -1;
    }

    if (mp4_crop_stsc_data(trak, 0) < 0) {
        return -1;
    }

    TSDebug(PLUGIN_NAME, "[mp4_update_stsc_atom] sample-to-chunk entries:%uD",
            trak->sample_to_chunk_entries);

    entry = trak->stsc_pos;
    end = trak->stsc_last;
    TSDebug(PLUGIN_NAME, "[mp4_update_stsc_atom] entry=%u,end=%u", entry, end);

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSC_DATA].reader);

    TSIOBufferReaderConsume(readerp, entry * sizeof(mp4_stsc_entry));

    while (entry < end) {
        chunk = mp4_reader_get_32value(readerp, offsetof(mp4_stsc_entry, chunk));
        chunk -= trak->start_chunk;
        mp4_reader_set_32value(readerp, offsetof(mp4_stsc_entry, chunk), chunk);
        entry++;
        TSIOBufferReaderConsume(readerp, sizeof(mp4_stsc_entry));
    }
    TSIOBufferReaderFree(readerp);

    atom_size = sizeof(mp4_stsc_atom)
                + (trak->stsc_last - trak->stsc_pos) * sizeof(mp4_stsc_entry);

    TSDebug(PLUGIN_NAME, "[mp4_update_stsc_atom] sizeof(mp4_stsc_atom) =%lu,atom_size=%llu",sizeof(mp4_stsc_atom), atom_size);

    trak->size += atom_size;

    TSIOBufferReaderConsume(trak->atoms[MP4_STSC_DATA].reader, trak->stsc_pos * sizeof(mp4_stsc_entry));


    new_avail = (trak->stsc_last - trak->stsc_pos) * sizeof(mp4_stsc_entry);
    copy_avail = TSIOBufferReaderAvail(copy_reader);
    if (copy_avail > 0) {
        TSIOBufferReaderConsume(copy_reader, copy_avail);
    }
    avail = TSIOBufferReaderAvail(trak->atoms[MP4_STSC_DATA].reader);
    if (new_avail > avail)
        new_avail = avail;
    TSDebug(PLUGIN_NAME, "[mp4_update_stsc_atom] avail=%lld, new_avail=%lld", avail, new_avail);
    TSIOBufferCopy(copy_buffer, trak->atoms[MP4_STSC_DATA].reader, new_avail, 0);
    TSIOBufferReaderConsume(trak->atoms[MP4_STSC_DATA].reader, avail);
    TSIOBufferCopy(trak->atoms[MP4_STSC_DATA].buffer, copy_reader, new_avail, 0);

    mp4_reader_set_32value(trak->atoms[MP4_STSC_ATOM].reader, offsetof(mp4_stsc_atom, size), atom_size);
    mp4_reader_set_32value(trak->atoms[MP4_STSC_ATOM].reader, offsetof(mp4_stsc_atom, entries),
                           trak->sample_to_chunk_entries);

    TSIOBufferReaderFree(readerp);
    return 0;
}

int
Mp4Meta::mp4_update_stsz_atom(Mp4Trak *trak) {

    size_t atom_size;
    uint32_t entries, pass, i, end_pass;
    TSIOBufferReader readerp;
    int64_t avail, copy_avail, new_avail;

    /*
     * mdia.minf.stbl.stsz updating requires trak->start_sample
     * from mdia.minf.stbl.stts which depends on value from mdia.mdhd
     * atom which may reside after mdia.minf
     */

    TSDebug(PLUGIN_NAME, "[mp4_update_stsz_atom] mp4 stsz atom update");

    if (trak->atoms[MP4_STSZ_DATA].buffer == nullptr) {
        return 0;
    }

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSZ_DATA].reader);

    entries = trak->sample_sizes_entries;

    if (trak->start_sample > entries) {
        TSDebug(PLUGIN_NAME, "[mp4_update_stsz_atom] start time is out mp4 stsz samples");
        return -1;
    }

    entries = entries - trak->start_sample;
    TSDebug(PLUGIN_NAME, "[mp4_update_stsz_atom] entries=%lu",entries);

    pass = trak->start_sample * sizeof(uint32_t);
    TSDebug(PLUGIN_NAME, "[mp4_update_stsz_atom] start_sample=%ld,pass=%lu", trak->start_sample, pass);
    TSIOBufferReaderConsume(readerp, pass - sizeof(uint32_t) * (trak->start_chunk_samples));

    for (i = 0; i < trak->start_chunk_samples; i++) {
        trak->start_chunk_samples_size += mp4_reader_get_32value(readerp, 0);
        TSIOBufferReaderConsume(readerp, sizeof(uint32_t));
    }

    TSDebug(PLUGIN_NAME, "[mp4_update_stsz_atom] chunk samples sizes:%uL", trak->start_chunk_samples_size);
    if (this->length) {
        if (trak->end_sample - trak->start_sample > entries) {
            TSDebug(PLUGIN_NAME, "[mp4_update_stsz_atom] end time is out mp4 stsz samples");
            TSIOBufferReaderFree(readerp);
            return -1;
        }

        entries = trak->end_sample - trak->start_sample;
        end_pass = entries * sizeof(uint32_t);
        TSDebug(PLUGIN_NAME, "[mp4_update_stsz_atom] end entries=%lu end_pass=%lu",entries, end_pass);
        TSIOBufferReaderConsume(readerp, end_pass - sizeof(uint32_t) * (trak->end_chunk_samples));

        for (i = 0; i < trak->end_chunk_samples; i++) {
            trak->end_chunk_samples_size += mp4_reader_get_32value(readerp, 0);
            TSIOBufferReaderConsume(readerp, sizeof(uint32_t));
        }

        TSDebug(PLUGIN_NAME, "[mp4_update_stsz_atom] mp4 stsz end_chunk_samples_size:%uL",
                trak->end_chunk_samples_size);
    }

    atom_size = sizeof(mp4_stsz_atom) + entries * sizeof(uint32_t);

    TSDebug(PLUGIN_NAME, "[mp4_update_stsz_atom] sizeof(mp4_stsz_atom) =%lu,atom_size=%llu",sizeof(mp4_stsz_atom), atom_size);

    trak->size += atom_size;


    mp4_reader_set_32value(trak->atoms[MP4_STSZ_ATOM].reader, offsetof(mp4_stsz_atom, size), atom_size);
    mp4_reader_set_32value(trak->atoms[MP4_STSZ_ATOM].reader, offsetof(mp4_stsz_atom, entries), entries);

    TSIOBufferReaderConsume(trak->atoms[MP4_STSZ_DATA].reader, pass);


    new_avail = entries * sizeof(uint32_t);
    copy_avail = TSIOBufferReaderAvail(copy_reader);
    if (copy_avail > 0) {
        TSIOBufferReaderConsume(copy_reader, copy_avail);
    }
    avail = TSIOBufferReaderAvail(trak->atoms[MP4_STSZ_DATA].reader);
    if (new_avail > avail)
        new_avail = avail;
    TSDebug(PLUGIN_NAME, "[mp4_update_stsz_atom] avail=%lld, new_avail=%lld", avail, new_avail);
    TSIOBufferCopy(copy_buffer, trak->atoms[MP4_STSZ_DATA].reader, new_avail, 0);
    TSIOBufferReaderConsume(trak->atoms[MP4_STSZ_DATA].reader, avail);
    TSIOBufferCopy(trak->atoms[MP4_STSZ_DATA].buffer, copy_reader, new_avail, 0);

    TSIOBufferReaderFree(readerp);
    return 0;
}

int
Mp4Meta::mp4_update_co64_atom(Mp4Trak *trak) {
    size_t atom_size;
    uint64_t entries;
    uint64_t pass, end_pass;
    TSIOBufferReader readerp;
    int64_t avail, copy_avail, new_avail;

    /*
     * mdia.minf.stbl.co64 updating requires trak->start_chunk
     * from mdia.minf.stbl.stsc which depends on value from mdia.mdhd
     * atom which may reside after mdia.minf
     */

    TSDebug(PLUGIN_NAME, "[mp4_update_co64_atom]  mp4 co64 atom update");

    if (trak->atoms[MP4_CO64_DATA].buffer == nullptr) {
        TSDebug(PLUGIN_NAME, "[mp4_update_co64_atom] no mp4 co64 atoms were found in ");
        return -1;
    }

    if (trak->start_chunk > trak->chunks) {
        TSDebug(PLUGIN_NAME, "[mp4_update_co64_atom] start time is out mp4 co64 chunks");
        return -1;
    }

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_CO64_DATA].reader);


    pass = trak->start_chunk * sizeof(uint64_t);

    TSIOBufferReaderConsume(readerp, pass);

    trak->start_offset = mp4_reader_get_64value(readerp, 0);
    trak->start_offset += trak->start_chunk_samples_size;
    mp4_reader_set_64value(readerp, 0, trak->start_offset);

    entries = 0;
    TSDebug(PLUGIN_NAME, "[mp4_update_co64_atom] start chunk offset:%lld", trak->start_offset);

    if (this->length) {

        if (trak->end_chunk > trak->chunks) {
            TSDebug(PLUGIN_NAME, "[mp4_update_co64_atom] end time is out mp4 co64 chunks");
            TSIOBufferReaderFree(readerp);
            return -1;
        }

//        entries = trak->end_chunk - trak->start_chunk;
        entries = trak->end_chunk;
        end_pass = entries * sizeof(uint64_t);
        if (entries) {
            TSIOBufferReaderConsume(readerp, end_pass);

            trak->end_offset = mp4_reader_get_64value(readerp, 0);
            trak->end_offset += trak->end_chunk_samples_size;

            TSDebug(PLUGIN_NAME, "[mp4_update_co64_atom] end chunk offset:%lld", trak->end_offset);
        }

    } else {
        entries = trak->chunks - trak->start_chunk;
        trak->end_offset = this->cl;
    }

    if (entries == 0) {
        trak->start_offset = this->cl;
        trak->end_offset = 0;
    }

    atom_size = sizeof(mp4_co64_atom) + entries * sizeof(uint64_t);

    TSDebug(PLUGIN_NAME, "[mp4_update_co64_atom] sizeof(mp4_co64_atom) =%lu,atom_size=%llu",sizeof(mp4_co64_atom), atom_size);

    trak->size += atom_size;

    TSIOBufferReaderConsume(trak->atoms[MP4_CO64_DATA].reader, pass);

    new_avail = entries * sizeof(uint64_t);
    copy_avail = TSIOBufferReaderAvail(copy_reader);
    if (copy_avail > 0) {
        TSIOBufferReaderConsume(copy_reader, copy_avail);
    }
    avail = TSIOBufferReaderAvail(trak->atoms[MP4_CO64_DATA].reader);
    if (new_avail > avail)
        new_avail = avail;
    TSDebug(PLUGIN_NAME, "[mp4_update_co64_atom] avail=%lld, new_avail=%lld", avail, new_avail);
    TSIOBufferCopy(copy_buffer, trak->atoms[MP4_CO64_DATA].reader, new_avail, 0);
    TSIOBufferReaderConsume(trak->atoms[MP4_CO64_DATA].reader, avail);
    TSIOBufferCopy(trak->atoms[MP4_CO64_DATA].buffer, copy_reader, new_avail, 0);

    mp4_reader_set_32value(trak->atoms[MP4_CO64_ATOM].reader, offsetof(mp4_co64_atom, size), atom_size);
    mp4_reader_set_32value(trak->atoms[MP4_CO64_ATOM].reader, offsetof(mp4_co64_atom, entries), entries);

    TSIOBufferReaderFree(readerp);
    return 0;
}

int//chunk offset box 定义了每个chunk在流媒体中的位置
Mp4Meta::mp4_update_stco_atom(Mp4Trak *trak) {
    size_t atom_size;
    uint32_t entries;
    uint64_t pass, end_pass;
    TSIOBufferReader readerp;
    int64_t avail, copy_avail, new_avail;

    /*
     * mdia.minf.stbl.stco updating requires trak->start_chunk
     * from mdia.minf.stbl.stsc which depends on value from mdia.mdhd
     * atom which may reside after mdia.minf
     */

    TSDebug(PLUGIN_NAME, "[mp4_update_stco_atom]  mp4 stco atom update");

    if (trak->atoms[MP4_STCO_DATA].buffer == nullptr) {
        TSDebug(PLUGIN_NAME, "[mp4_update_stco_atom] no mp4 stco atoms were found in ");
        return -1;
    }

    if (trak->start_chunk > trak->chunks) {
        TSDebug(PLUGIN_NAME, "[mp4_update_stco_atom] start time is out mp4 stco chunks");
        return -1;
    }

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STCO_DATA].reader);
    TSDebug(PLUGIN_NAME, "[mp4_update_stco_atom] avail=%lld", TSIOBufferReaderAvail(trak->atoms[MP4_STCO_DATA].reader));
    pass = trak->start_chunk * sizeof(uint32_t);
    TSDebug(PLUGIN_NAME, "[mp4_update_stco_atom] start_chunk=%lu, end_chunk=%lu,chunk=%lu, pass=%llu", trak->start_chunk,
            trak->end_chunk, trak->chunks, pass);
    TSIOBufferReaderConsume(readerp, pass);

    trak->start_offset = mp4_reader_get_32value(readerp, 0);
    trak->start_offset += trak->start_chunk_samples_size;
    mp4_reader_set_32value(readerp, 0, trak->start_offset);


    TSDebug(PLUGIN_NAME, "[mp4_update_stco_atom] start chunk offset:%llu", trak->start_offset);
    entries = 0;
//    end_pass = 0;
    if (this->length) {

        if (trak->end_chunk > trak->chunks) {
            TSDebug(PLUGIN_NAME, "[mp4_update_stco_atom] end time is out mp4 stco chunks");
            TSIOBufferReaderFree(readerp);
            return -1;
        }

//        entries = trak->end_chunk - trak->start_chunk;
        entries = trak->end_chunk;
        end_pass = entries * sizeof(uint32_t);
        if (entries) {
            TSIOBufferReaderConsume(readerp, end_pass);
            TSDebug(PLUGIN_NAME, "[mp4_update_stco_atom] end_pass=%llu, entries=%llu", end_pass, entries);
            trak->end_offset = mp4_reader_get_32value(readerp, 0);
            trak->end_offset += trak->end_chunk_samples_size;

            TSDebug(PLUGIN_NAME, "[mp4_update_stco_atom] end chunk offset:%llu", trak->end_offset);
        }

    } else {
        entries = trak->chunks - trak->start_chunk;
        trak->end_offset = this->cl;
    }

    if (entries == 0) {
        trak->start_offset = this->cl;
        trak->end_offset = 0;
    }

    atom_size = sizeof(mp4_stco_atom) + entries * sizeof(uint32_t);

    TSDebug(PLUGIN_NAME, "[mp4_update_stco_atom] sizeof(mp4_stco_atom) =%lu,atom_size=%llu",sizeof(mp4_stco_atom), atom_size);

    trak->size += atom_size;

    TSIOBufferReaderConsume(trak->atoms[MP4_STCO_DATA].reader, pass);

    new_avail = entries * sizeof(uint32_t);
    copy_avail = TSIOBufferReaderAvail(copy_reader);
    if (copy_avail > 0) {
        TSIOBufferReaderConsume(copy_reader, copy_avail);
    }
    avail = TSIOBufferReaderAvail(trak->atoms[MP4_STCO_DATA].reader);
    if (new_avail > avail)
        new_avail = avail;
    TSDebug(PLUGIN_NAME, "[mp4_update_stco_atom] avail=%lld, new_avail=%lld", avail, new_avail);
    TSIOBufferCopy(copy_buffer, trak->atoms[MP4_STCO_DATA].reader, new_avail, 0);
    TSIOBufferReaderConsume(trak->atoms[MP4_STCO_DATA].reader, avail);
    TSIOBufferCopy(trak->atoms[MP4_STCO_DATA].buffer, copy_reader, new_avail, 0);

    mp4_reader_set_32value(trak->atoms[MP4_STCO_ATOM].reader, offsetof(mp4_stco_atom, size), atom_size);
    mp4_reader_set_32value(trak->atoms[MP4_STCO_ATOM].reader, offsetof(mp4_stco_atom, entries), entries);

    TSIOBufferReaderFree(readerp);
    return 0;
}

int
Mp4Meta::mp4_update_stbl_atom(Mp4Trak *trak) {
    trak->size += sizeof(mp4_atom_header);
    mp4_reader_set_32value(trak->atoms[MP4_STBL_ATOM].reader, 0, trak->size);

    return 0;
}

int
Mp4Meta::mp4_update_minf_atom(Mp4Trak *trak) {
    trak->size += sizeof(mp4_atom_header) + trak->vmhd_size + trak->smhd_size + trak->dinf_size;

    mp4_reader_set_32value(trak->atoms[MP4_MINF_ATOM].reader, 0, trak->size);

    return 0;
}

int
Mp4Meta::mp4_update_mdia_atom(Mp4Trak *trak) {
    trak->size += sizeof(mp4_atom_header);
    mp4_reader_set_32value(trak->atoms[MP4_MDIA_ATOM].reader, 0, trak->size);

    return 0;
}

int
Mp4Meta::mp4_update_trak_atom(Mp4Trak *trak) {
    trak->size += sizeof(mp4_atom_header);
    mp4_reader_set_32value(trak->atoms[MP4_TRAK_ATOM].reader, 0, trak->size);

    return 0;
}

int
Mp4Meta::mp4_adjust_co64_atom(Mp4Trak *trak, off_t adjustment) {
    int64_t pos, avail, offset;
    TSIOBufferReader readerp;

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_CO64_DATA].reader);
    avail = TSIOBufferReaderAvail(readerp);

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
Mp4Meta::mp4_adjust_stco_atom(Mp4Trak *trak, int32_t adjustment) {
    int64_t pos, avail, offset;
    TSIOBufferReader readerp;

    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STCO_DATA].reader);
    avail = TSIOBufferReaderAvail(readerp);
    TSDebug(PLUGIN_NAME, "[mp4_adjust_stco_atom] avail=%ld", avail);
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
Mp4Meta::mp4_update_mdat_atom(int64_t start_offset, int64_t end_offset) {
    int64_t atom_data_size;
    int64_t atom_size;
    int64_t atom_header_size;
    u_char *atom_header;

    atom_data_size = end_offset > start_offset ? end_offset - start_offset : this->cl - start_offset;//剩余的都是mdat
    this->start_pos = start_offset;
    this->end_pos = end_offset;
    TSDebug(PLUGIN_NAME, "[mp4_update_mdat_atom] this->start_pos= %ld, atom_data_size=%ld", this->start_pos,
            atom_data_size);
    TSDebug(PLUGIN_NAME, "[mp4_update_mdat_atom] this->end_pos= %ld", this->end_pos);
    atom_header = mdat_atom_header;

    if (atom_data_size > 0xffffffff) {
        atom_size = 1;
        atom_header_size = sizeof(mp4_atom_header64);
        mp4_set_64value(atom_header + sizeof(mp4_atom_header), sizeof(mp4_atom_header64) + atom_data_size);

    } else {
        atom_size = sizeof(mp4_atom_header) + atom_data_size;
        atom_header_size = sizeof(mp4_atom_header);
    }

    this->content_length += atom_header_size + atom_data_size;
//    this->content_length += atom_header_size + 1024*1024*1;

    TSDebug(PLUGIN_NAME, "[mp4_update_mdat_atom] atom_header_size=%ld content_length=%ld",
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
Mp4Meta::mp4_find_key_sample(uint32_t start_sample, Mp4Trak *trak) {
    uint32_t i;
    uint32_t sample, prev_sample, entries;
    TSIOBufferReader readerp;

    if (trak->atoms[MP4_STSS_DATA].buffer == nullptr) {
        return start_sample;
    }

    prev_sample = 1;
    entries = trak->sync_samples_entries;
    //mp4_find_key_sample sync_samples_entries=75
    TSDebug(PLUGIN_NAME, "mp4_find_key_sample sync_samples_entries=%u", entries);
    readerp = TSIOBufferReaderClone(trak->atoms[MP4_STSS_DATA].reader);

    for (i = 0; i < entries; i++) {
        sample = (uint32_t) mp4_reader_get_32value(readerp, 0);
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
Mp4Meta::mp4_update_mvhd_duration() {
    int64_t need;
    uint64_t duration, cut, end_cut;
    mp4_mvhd_atom *mvhd;
    mp4_mvhd64_atom mvhd64;

    need = TSIOBufferReaderAvail(mvhd_atom.reader);

    if (need > (int64_t) sizeof(mp4_mvhd64_atom)) {
        need = sizeof(mp4_mvhd64_atom);
    }

    memset(&mvhd64, 0, sizeof(mvhd64));
    IOBufferReaderCopy(mvhd_atom.reader, &mvhd64, need);
    mvhd = (mp4_mvhd_atom *) &mvhd64;

    if (this->rs > 0) {
        cut = (uint64_t) (this->rs * this->timescale / 1000);

    } else {
        cut = this->start * this->timescale / 1000;
    }

    end_cut = 0;
    if (this->end > 0) {
        if (this->end_rs > 0) {
            end_cut = (uint64_t) (this->end_rs * this->timescale / 1000);

        } else {
            end_cut = this->end * this->timescale / 1000;
        }
    }
    TSDebug(PLUGIN_NAME, "[mp4_update_mvhd_duration] end_cut=%llu, cut=%llu, this->timescale=%llu", end_cut, cut,
            this->timescale);
    if (mvhd->version[0] == 0) {
        duration = mp4_get_32value(mvhd->duration);
        if (this->end > 0 && end_cut > 0 && end_cut > cut) {
            duration = end_cut - cut;
        } else {
            duration -= cut;
        }
        TSDebug(PLUGIN_NAME, "[mp4_update_mvhd_duration] duration=%llu", (uint32_t) duration);
        mp4_reader_set_32value(mvhd_atom.reader, offsetof(mp4_mvhd_atom, duration), (uint32_t) duration);
        TSDebug(PLUGIN_NAME, "[mp4_update_mvhd_duration] timescale=%llu",
                mp4_reader_get_32value(mvhd_atom.reader, offsetof(mp4_mvhd_atom, timescale)));

    } else { // 64-bit duration
        duration = mp4_get_64value(mvhd64.duration);
        if (this->end > 0 && end_cut > 0 && end_cut > cut) {
            duration = end_cut - cut;
        } else {
            duration -= cut;
        }
        TSDebug(PLUGIN_NAME, "[mp4_update_mvhd_duration] duration=%llu", duration);
        mp4_reader_set_64value(mvhd_atom.reader, offsetof(mp4_mvhd64_atom, duration), duration);
        TSDebug(PLUGIN_NAME, "[mp4_update_mvhd_duration] timescale=%llu",
                mp4_reader_get_64value(mvhd_atom.reader, offsetof(mp4_mvhd64_atom, timescale)));
    }
//    time = duration / timescale
    //timescale
    TSDebug(PLUGIN_NAME, "[mp4_update_mvhd_duration] duration=%llu, timescale=", duration);
}

void
Mp4Meta::mp4_update_tkhd_duration(Mp4Trak *trak) {
    int64_t need, cut, end_cut;
    mp4_tkhd_atom *tkhd_atom;
    mp4_tkhd64_atom tkhd64_atom;
    int64_t duration;

    need = TSIOBufferReaderAvail(trak->atoms[MP4_TKHD_ATOM].reader);

    if (need > (int64_t) sizeof(mp4_tkhd64_atom)) {
        need = sizeof(mp4_tkhd64_atom);
    }

    memset(&tkhd64_atom, 0, sizeof(tkhd64_atom));
    IOBufferReaderCopy(trak->atoms[MP4_TKHD_ATOM].reader, &tkhd64_atom, need);
    tkhd_atom = (mp4_tkhd_atom *) &tkhd64_atom;

    if (this->rs > 0) {
        cut = (uint64_t) (this->rs * this->timescale / 1000);

    } else {
        cut = this->start * this->timescale / 1000;
    }
    end_cut = 0;
    if (this->end > 0) {
        if (this->end_rs > 0) {
            end_cut = (uint64_t) (this->end_rs * this->timescale / 1000);
        } else {
            end_cut = this->end * this->timescale / 1000;
        }
    }
    TSDebug(PLUGIN_NAME, "[mp4_update_tkhd_duration] end_cut=%llu, cut=%llu, this->timescale=%llu", end_cut, cut,
            this->timescale);
    if (tkhd_atom->version[0] == 0) {
        duration = mp4_get_32value(tkhd_atom->duration);
        if (this->end > 0 && end_cut > 0 && end_cut > cut) {
            duration = end_cut - cut;
        } else {
            duration -= cut;
        }

        mp4_reader_set_32value(trak->atoms[MP4_TKHD_ATOM].reader, offsetof(mp4_tkhd_atom, duration), duration);
        TSDebug(PLUGIN_NAME, "[mp4_update_tkhd_duration] duration=%llu",
                mp4_reader_get_32value(trak->atoms[MP4_TKHD_ATOM].reader, offsetof(mp4_tkhd_atom, duration)));

    } else {
        duration = mp4_get_64value(tkhd64_atom.duration);
        if (this->end > 0 && end_cut > 0 && end_cut > cut) {
            duration = end_cut - cut;
        } else {
            duration -= cut;
        }
        mp4_reader_set_64value(trak->atoms[MP4_TKHD_ATOM].reader, offsetof(mp4_tkhd64_atom, duration), duration);
        TSDebug(PLUGIN_NAME, "[mp4_update_tkhd_duration] duration=%llu",
                mp4_reader_get_64value(trak->atoms[MP4_TKHD_ATOM].reader, offsetof(mp4_tkhd64_atom, duration)));

    }
}

void
Mp4Meta::mp4_update_mdhd_duration(Mp4Trak *trak) {
    int64_t duration, need, cut, end_cut;
    mp4_mdhd_atom *mdhd;
    mp4_mdhd64_atom mdhd64;

    memset(&mdhd64, 0, sizeof(mp4_mdhd64_atom));

    need = TSIOBufferReaderAvail(trak->atoms[MP4_MDHD_ATOM].reader);

    if (need > (int64_t) sizeof(mp4_mdhd64_atom)) {
        need = sizeof(mp4_mdhd64_atom);
    }

    IOBufferReaderCopy(trak->atoms[MP4_MDHD_ATOM].reader, &mdhd64, need);
    mdhd = (mp4_mdhd_atom *) &mdhd64;

    if (this->rs > 0) {
        cut = (int64_t) (this->rs * trak->timescale / 1000);
    } else {
        cut = this->start * trak->timescale / 1000;
    }

    end_cut = 0;
    if (this->end > 0) {
        if (this->end_rs > 0) {
            end_cut = (int64_t) (this->end_rs * trak->timescale / 1000);
        } else {
            end_cut = this->end * trak->timescale / 1000;
        }
    }
    TSDebug(PLUGIN_NAME, "[mp4_update_mdhd_duration] end_cut=%llu, cut=%llu, trak->timescale=%llu", end_cut, cut,
            trak->timescale);
    if (mdhd->version[0] == 0) {
        duration = mp4_get_32value(mdhd->duration);
        if (this->end > 0 && end_cut > 0 && end_cut > cut) {
            duration = end_cut - cut;
        } else {
            duration -= cut;
        }
        mp4_reader_set_32value(trak->atoms[MP4_MDHD_ATOM].reader, offsetof(mp4_mdhd_atom, duration), duration);
        TSDebug(PLUGIN_NAME, "[mp4_update_mdhd_duration] duration=%llu",
                mp4_reader_get_32value(trak->atoms[MP4_MDHD_ATOM].reader, offsetof(mp4_mdhd_atom, duration)));
    } else {
        duration = mp4_get_64value(mdhd64.duration);
        if (this->end > 0 && end_cut > 0 && end_cut > cut) {
            duration = end_cut - cut;
        } else {
            duration -= cut;
        }
        mp4_reader_set_64value(trak->atoms[MP4_MDHD_ATOM].reader, offsetof(mp4_mdhd64_atom, duration), duration);
        TSDebug(PLUGIN_NAME, "[mp4_update_mdhd_duration] duration=%llu",
                mp4_reader_get_64value(trak->atoms[MP4_MDHD_ATOM].reader, offsetof(mp4_mdhd64_atom, duration)));

    }
}

static void
mp4_reader_set_32value(TSIOBufferReader readerp, int64_t offset, uint32_t n) {
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
            ptr = (u_char *) (const_cast<char *>(start) + offset);

            while (pos < 4 && left > 0) {
                *ptr++ = (u_char) ((n) >> ((3 - pos) * 8));
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
mp4_reader_set_64value(TSIOBufferReader readerp, int64_t offset, uint64_t n) {
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
            ptr = (u_char *) (const_cast<char *>(start) + offset);

            while (pos < 8 && left > 0) {
                *ptr++ = (u_char) ((n) >> ((7 - pos) * 8));
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
mp4_reader_get_32value(TSIOBufferReader readerp, int64_t offset) {
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
            ptr = (u_char *) (start + offset);

            while (pos < 4 && left > 0) {
                res[3 - pos] = *ptr++;
                pos++;
                left--;
            }

            if (pos >= 4) {
                return *(uint32_t *) res;
            }

            offset = 0;
        }

        blk = TSIOBufferBlockNext(blk);
    }

    return -1;
}

static uint64_t
mp4_reader_get_64value(TSIOBufferReader readerp, int64_t offset) {
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
            ptr = (u_char *) (start + offset);

            while (pos < 8 && left > 0) {
                res[7 - pos] = *ptr++;
                pos++;
                left--;
            }

            if (pos >= 8) {
                return *(uint64_t *) res;
            }

            offset = 0;
        }

        blk = TSIOBufferBlockNext(blk);
    }

    return -1;
}

static int64_t
IOBufferReaderCopy(TSIOBufferReader readerp, void *buf, int64_t length) {
    int64_t avail, need, n;
    const char *start;
    TSIOBufferBlock blk;

    n = 0;
    blk = TSIOBufferReaderStart(readerp);

    while (blk) {
        start = TSIOBufferBlockReadStart(blk, readerp, &avail);
        need = length < avail ? length : avail;

        if (need > 0) {
            memcpy((char *) buf + n, start, need);
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
