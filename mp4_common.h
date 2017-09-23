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

#ifndef _MP4_COMMON_H
#define _MP4_COMMON_H

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>

#include <ts/ts.h>
#include <ts/experimental.h>
#include <ts/remap.h>
#include "mp4_meta.h"

class IOHandle
{
public:
  IOHandle() : vio(NULL), buffer(NULL), reader(NULL){};

  ~IOHandle()
  {
    if (reader) {
      TSIOBufferReaderFree(reader);
      reader = NULL;
    }

    if (buffer) {
      TSIOBufferDestroy(buffer);
      buffer = NULL;
    }
  }

public:
  TSVIO vio;
  TSIOBuffer buffer;
  TSIOBufferReader reader;
};

class Mp4TransformContext
{
public:
  Mp4TransformContext(float offset, float end_offset, int64_t cl)
    : total(0), start_tail(0), end_tail(0), start_pos(0), end_pos(0), content_length(0), meta_length(0), parse_over(false), raw_transform(false)
  {
    res_buffer = TSIOBufferCreate();
    res_reader = TSIOBufferReaderAlloc(res_buffer);
    dup_reader = TSIOBufferReaderAlloc(res_buffer);

    mm.start = offset * 1000;
    mm.end =   end_offset * 1000;
    if (mm.end > 0) {
      if (mm.start < 0) {
        mm.start = 0;
      }

      if (mm.end > mm.start) {
        mm.length = mm.end - mm.start;
      }
    }
    mm.cl    = cl;
  }

  ~Mp4TransformContext()
  {
    if (res_reader) {
      TSIOBufferReaderFree(res_reader);
    }

    if (dup_reader) {
      TSIOBufferReaderFree(dup_reader);
    }

    if (res_buffer) {
      TSIOBufferDestroy(res_buffer);
    }
  }

public:
  IOHandle output;
  Mp4Meta mm;
  int64_t total;
  int64_t start_tail;
  int64_t end_tail;
  int64_t start_pos;//start 起始结束丢弃的位置
  int64_t end_pos; //end 丢弃的位置
  int64_t content_length;
  int64_t meta_length;

  TSIOBuffer res_buffer;
  TSIOBufferReader res_reader;
  TSIOBufferReader dup_reader;

  bool parse_over;
  bool raw_transform;
};

class Mp4Context
{
public:
  Mp4Context(float s, float e) : start(s), end(e),cl(0), mtc(NULL), transform_added(false){};

  ~Mp4Context()
  {
    if (mtc) {
      delete mtc;
      mtc = NULL;
    }
  }

public:
  float start;
  float end;
  int64_t cl;

  Mp4TransformContext *mtc;

  bool transform_added;
};

#endif
