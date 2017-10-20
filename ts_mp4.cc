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

#include "mp4_common.h"

static int mp4_handler(TSCont contp, TSEvent event, void *edata);
static void mp4_cache_lookup_complete(Mp4Context *mc, TSHttpTxn txnp);
static void mp4_read_response(Mp4Context *mc, TSHttpTxn txnp);
static void mp4_add_transform(Mp4Context *mc, TSHttpTxn txnp);
static int mp4_transform_entry(TSCont contp, TSEvent event, void *edata);
static int mp4_transform_handler(TSCont contp, Mp4Context *mc);
static int mp4_parse_meta(Mp4TransformContext *mtc, bool body_complete);

TSReturnCode
TSRemapInit(TSRemapInterface *api_info, char *errbuf, int errbuf_size)
{
  if (!api_info) {
    snprintf(errbuf, errbuf_size, "[TSRemapInit] - Invalid TSRemapInterface argument");
    return TS_ERROR;
  }

  if (api_info->size < sizeof(TSRemapInterface)) {
    snprintf(errbuf, errbuf_size, "[TSRemapInit] - Incorrect size of TSRemapInterface structure");
    return TS_ERROR;
  }

  return TS_SUCCESS;
}

TSReturnCode
TSRemapNewInstance(int argc, char ** /* argv ATS_UNUSED */, void **ih, char *errbuf, int errbuf_size)
{
  if (argc > 2) {
    snprintf(errbuf, errbuf_size, "[TSRemapNewInstance] - Argument should be removed");
  }

  *ih = nullptr;
  return TS_SUCCESS;
}

void
TSRemapDeleteInstance(void * /* ih ATS_UNUSED */)
{
  return;
}

TSRemapStatus
TSRemapDoRemap(void * /* ih ATS_UNUSED */, TSHttpTxn rh, TSRemapRequestInfo *rri)
{
  const char *method, *query, *path;
  const char *f_start, *f_end;
  int method_len, query_len, path_len;
  float start, end;
  TSMLoc ae_field, range_field;
  TSCont contp;
  Mp4Context *mc;
  bool is_find;

  method = TSHttpHdrMethodGet(rri->requestBufp, rri->requestHdrp, &method_len);
  if (method != TS_HTTP_METHOD_GET) {
    return TSREMAP_NO_REMAP;
  }

  // check suffix
  path = TSUrlPathGet(rri->requestBufp, rri->requestUrl, &path_len);

  if (path == nullptr || path_len <= 4) {
    return TSREMAP_NO_REMAP;

  } else if (strncasecmp(path + path_len - 4, ".mp4", 4) != 0) {
    return TSREMAP_NO_REMAP;
  }

  start = 0;
  end = 0;
  is_find = false;
  query = TSUrlHttpQueryGet(rri->requestBufp, rri->requestUrl, &query_len);
  TSDebug(PLUGIN_NAME, "TSRemapDoRemap query=%s!",query);
  if(!query) {
    TSDebug(PLUGIN_NAME, "TSRemapDoRemap query is null!");
    return TSREMAP_NO_REMAP;
  }

  f_start = strcasestr(query, "start=");
  if (f_start) {
      start = strtod(f_start + 6, NULL);
      is_find = true;
  }

  f_end = strcasestr(query, "end=");

  if(f_end) {
    end = strtod(f_end + 4, NULL);
    is_find = true;
  }

  if (!is_find) {
    TSDebug(PLUGIN_NAME, "TSRemapDoRemap not found start= or end=");
    return TSREMAP_NO_REMAP;
  }

  TSDebug(PLUGIN_NAME, "TSRemapDoRemap start=%lf, end=%lf", start, end);
  if (start < 0 || end < 0 || (start > 0 && end > 0 && start >= end)) {
    return TSREMAP_NO_REMAP;
  }


  //remove query
//  if (TSUrlHttpQuerySet(rri->requestBufp, rri->requestUrl, "", -1) == TS_ERROR) {
//    return TSREMAP_NO_REMAP;
//  }

  // remove Accept-Encoding
  ae_field = TSMimeHdrFieldFind(rri->requestBufp, rri->requestHdrp, TS_MIME_FIELD_ACCEPT_ENCODING, TS_MIME_LEN_ACCEPT_ENCODING);
  if (ae_field) {
    TSMimeHdrFieldDestroy(rri->requestBufp, rri->requestHdrp, ae_field);
    TSHandleMLocRelease(rri->requestBufp, rri->requestHdrp, ae_field);
  }

  // remove range
  range_field = TSMimeHdrFieldFind(rri->requestBufp, rri->requestHdrp, TS_MIME_FIELD_RANGE, TS_MIME_LEN_RANGE);
  if (range_field) {
//      TSDebug(PLUGIN_NAME, "TSRemapDoRemap range request");
//      return TSREMAP_NO_REMAP;
    TSMimeHdrFieldDestroy(rri->requestBufp, rri->requestHdrp, range_field);
    TSHandleMLocRelease(rri->requestBufp, rri->requestHdrp, range_field);
  }

  mc    = new Mp4Context(start, end);
  contp = TSContCreate(mp4_handler, nullptr);
  TSContDataSet(contp, mc);

  TSHttpTxnHookAdd(rh, TS_HTTP_CACHE_LOOKUP_COMPLETE_HOOK, contp);
  TSHttpTxnHookAdd(rh, TS_HTTP_READ_RESPONSE_HDR_HOOK, contp);
  TSHttpTxnHookAdd(rh, TS_HTTP_TXN_CLOSE_HOOK, contp);
  return TSREMAP_NO_REMAP;
}

static int
mp4_handler(TSCont contp, TSEvent event, void *edata)
{
  TSHttpTxn txnp;
  Mp4Context *mc;

  txnp = (TSHttpTxn)edata;
  mc   = (Mp4Context *)TSContDataGet(contp);

  switch (event) {
  case TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE:
    mp4_cache_lookup_complete(mc, txnp);
    TSDebug(PLUGIN_NAME, "\tEvent is TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE");
    break;

  case TS_EVENT_HTTP_READ_RESPONSE_HDR:
    mp4_read_response(mc, txnp);
    TSDebug(PLUGIN_NAME, "\tEvent is TS_EVENT_HTTP_READ_RESPONSE_HDR");
    break;

  case TS_EVENT_HTTP_TXN_CLOSE:
    TSDebug(PLUGIN_NAME, "TS_EVENT_HTTP_TXN_CLOSE");
    delete mc;
    TSContDestroy(contp);
    break;

  default:
    break;
  }

  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
  return 0;
}

static void
mp4_cache_lookup_complete(Mp4Context *mc, TSHttpTxn txnp)
{
  TSMBuffer bufp;
  TSMLoc hdrp;
  TSMLoc cl_field;
  TSHttpStatus code;
  int obj_status;
  int64_t n;

  if (TSHttpTxnCacheLookupStatusGet(txnp, &obj_status) == TS_ERROR) {
    TSError("[%s] Couldn't get cache status of object", __FUNCTION__);
    return;
  }

  if (obj_status != TS_CACHE_LOOKUP_HIT_STALE && obj_status != TS_CACHE_LOOKUP_HIT_FRESH) {
    return;
  }

  if (TSHttpTxnCachedRespGet(txnp, &bufp, &hdrp) != TS_SUCCESS) {
    TSError("[%s] Couldn't get cache resp", __FUNCTION__);
    return;
  }

  code = TSHttpHdrStatusGet(bufp, hdrp);
  if (code != TS_HTTP_STATUS_OK) {
    goto release;
  }

  n = 0;

  cl_field = TSMimeHdrFieldFind(bufp, hdrp, TS_MIME_FIELD_CONTENT_LENGTH, TS_MIME_LEN_CONTENT_LENGTH);
  if (cl_field) {
    n = TSMimeHdrFieldValueInt64Get(bufp, hdrp, cl_field, -1);
    TSHandleMLocRelease(bufp, hdrp, cl_field);
  }

  if (n <= 0) {
    goto release;
  }

  TSDebug(PLUGIN_NAME, "[mp4_cache_lookup_complete]  content_length=%ld", n);
  mc->cl = n;
  mp4_add_transform(mc, txnp);

release:

  TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdrp);
}

static void
mp4_read_response(Mp4Context *mc, TSHttpTxn txnp)
{
  TSMBuffer bufp;
  TSMLoc hdrp;
  TSMLoc cl_field;
  TSHttpStatus status;
  int64_t n;

  if (TSHttpTxnServerRespGet(txnp, &bufp, &hdrp) != TS_SUCCESS) {
    TSError("[%s] could not get request os data", __FUNCTION__);
    return;
  }

  status = TSHttpHdrStatusGet(bufp, hdrp);
  if (status != TS_HTTP_STATUS_OK) {
    goto release;
  }

  n        = 0;
  cl_field = TSMimeHdrFieldFind(bufp, hdrp, TS_MIME_FIELD_CONTENT_LENGTH, TS_MIME_LEN_CONTENT_LENGTH);
  if (cl_field) {
    n = TSMimeHdrFieldValueInt64Get(bufp, hdrp, cl_field, -1);
    TSHandleMLocRelease(bufp, hdrp, cl_field);
  }

  if (n <= 0) {
    goto release;
  }

  TSDebug(PLUGIN_NAME, "[mp4_cache_lookup_complete]  content_length=%ld", n);

  mc->cl = n;
  mp4_add_transform(mc, txnp);

release:

  TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdrp);
}

static void
mp4_add_transform(Mp4Context *mc, TSHttpTxn txnp)
{
  TSVConn connp;

  if (!mc)
    return;

  if (mc->start <= 0) {
    mc->start = 0;
  }
  if (mc->end <= 0) {
    mc->end = 0;
  }

  if (mc->end <= mc->start) {
      mc->end = 0;
  }

  if(mc->start == 0 && mc->end == 0) {
    return;
  }

  if (mc->transform_added) {
    return;
  }

  mc->mtc = new Mp4TransformContext(mc->start, mc->end, mc->cl);

  TSDebug(PLUGIN_NAME, "[mp4_add_transform] start=%ld, end=%ld, cl=%ld",mc->start, mc->end, mc->cl);

  TSHttpTxnUntransformedRespCache(txnp, 1);
  TSHttpTxnTransformedRespCache(txnp, 0);

  connp = TSTransformCreate(mp4_transform_entry, txnp);
  TSContDataSet(connp, mc);
  TSHttpTxnHookAdd(txnp, TS_HTTP_RESPONSE_TRANSFORM_HOOK, connp);

  mc->transform_added = true;
}

static int
mp4_transform_entry(TSCont contp, TSEvent event, void * /* edata ATS_UNUSED */)
{
  TSVIO input_vio;
  Mp4Context *mc = (Mp4Context *)TSContDataGet(contp);

  if (TSVConnClosedGet(contp)) {
    TSContDestroy(contp);
    TSDebug(PLUGIN_NAME, "\tVConn is closed");
    return 0;
  }

  switch (event) {
  case TS_EVENT_ERROR:
    TSDebug(PLUGIN_NAME, "\tEvent is TS_EVENT_ERROR");
    input_vio = TSVConnWriteVIOGet(contp);
    TSContCall(TSVIOContGet(input_vio), TS_EVENT_ERROR, input_vio);
    break;

  case TS_EVENT_VCONN_WRITE_COMPLETE:
    TSDebug(PLUGIN_NAME, "\tEvent is TS_EVENT_VCONN_WRITE_COMPLETE");
    TSVConnShutdown(TSTransformOutputVConnGet(contp), 0, 1);
    break;

  case TS_EVENT_VCONN_WRITE_READY:
  default:
    TSDebug(PLUGIN_NAME, "\tEvent is TS_EVENT_VCONN_WRITE_READY");
    mp4_transform_handler(contp, mc);
    break;
  }

  return 0;
}

static int
mp4_transform_handler(TSCont contp, Mp4Context *mc)
{
    TSDebug(PLUGIN_NAME, "[mp4_transform_handler] start");

  TSVConn output_conn;
  TSVIO input_vio;
  TSIOBufferReader input_reader;
  int64_t avail, toread, need, upstream_done;
  int ret;
  bool write_down;
  Mp4TransformContext *mtc;

  mtc = mc->mtc;

  output_conn  = TSTransformOutputVConnGet(contp);
  input_vio    = TSVConnWriteVIOGet(contp);
  input_reader = TSVIOReaderGet(input_vio);

  if (!TSVIOBufferGet(input_vio)) {
    if (mtc->output.buffer) {
      TSVIONBytesSet(mtc->output.vio, mtc->total);
      TSVIOReenable(mtc->output.vio);
      TSDebug(PLUGIN_NAME, "[mp4_transform_handler] !input_buff Done Get=%ld, total=%ld",
                    TSVIONDoneGet(mtc->output.vio), mtc->total);
    }
    return 1;
  }

  avail         = TSIOBufferReaderAvail(input_reader);//可读
  upstream_done = TSVIONDoneGet(input_vio);//已经完成了多少

   toread     = TSVIONTodoGet(input_vio);//还剩下多少未读
   TSDebug(PLUGIN_NAME, "[mp4_transform_handler] before write toread is %ld", toread);

  TSIOBufferCopy(mtc->res_buffer, input_reader, avail, 0);
  TSIOBufferReaderConsume(input_reader, avail);
  TSVIONDoneSet(input_vio, upstream_done + avail);

  toread     = TSVIONTodoGet(input_vio);//还剩下多少未读

  TSDebug(PLUGIN_NAME, "[mp4_transform_handler] after write toread is %ld", toread);
  TSDebug(PLUGIN_NAME, "[mp4_transform_handler] input_vio avail is %ld", avail);

  write_down = false;//是否有数据写入

  if (!mtc->parse_over) {//解析mp4头
    TSDebug(PLUGIN_NAME, "[mp4_transform_handler] in parse_over toread-avail=%ld",(toread-avail));
    ret = mp4_parse_meta(mtc, toread <= 0);
    TSDebug(PLUGIN_NAME, "[mp4_transform_handler] ret=%d",ret);
    if (ret == 0) {
      goto trans;
    }

    mtc->parse_over    = true;
    mtc->output.buffer = TSIOBufferCreate();
    mtc->output.reader = TSIOBufferReaderAlloc(mtc->output.buffer);

    if (ret < 0) {//解析失败的话，就将整个文件返回
      mtc->output.vio = TSVConnWrite(output_conn, contp, mtc->output.reader, mc->cl);// cl 为原始文件长度
      mtc->raw_transform = true;

    } else {//解析成功的话，就按照之前的start, end 的流程走
      mtc->output.vio = TSVConnWrite(output_conn, contp, mtc->output.reader, mtc->content_length);//修剪之后的文件长度
    }
  }

  TSDebug(PLUGIN_NAME, "[mp4_transform_handler] out parse_over");

    //mtc->res_reader 从开始到start_pos 的数据都被存储了,修改和读取的信息都是另外一个计数dup_reader容器
  avail = TSIOBufferReaderAvail(mtc->res_reader);

  if (mtc->raw_transform) {//解析meta失败
    if (avail > 0) {
      TSIOBufferCopy(mtc->output.buffer, mtc->res_reader, avail, 0);
      TSIOBufferReaderConsume(mtc->res_reader, avail);
      mtc->total += avail;
      write_down = true;
    }

  } else {//解析mp4 meta，并且修改成功
    // copy the new meta data, 如果total < meta_length 说明还没有copy 过
    if (mtc->total < mtc->meta_length) {
      TSIOBufferCopy(mtc->output.buffer, mtc->mm.out_handle.reader, mtc->meta_length, 0);
      mtc->total += mtc->meta_length;
      write_down = true;
    }

    // ignore useless part, 忽视无用的部分，  tail 为丢弃的结束位置
    if (mtc->start_pos < mtc->start_tail) {
      avail = TSIOBufferReaderAvail(mtc->res_reader);
      need  = mtc->start_tail - mtc->start_pos;
      if (need > avail) {
        need = avail;
      }

      if (need > 0) {
        TSIOBufferReaderConsume(mtc->res_reader, need);
        mtc->start_pos += need;
      }
    }

    // copy the video & audio data  后面从此地方入手，操作end
    if(mtc->end_tail > 0) {
      if (mtc->start_pos >= mtc->start_tail && mtc->start_pos <= mtc->end_tail) {
        avail = TSIOBufferReaderAvail(mtc->res_reader);
        need = mtc->end_tail - mtc->start_pos;
        if(need > avail) {
          need = avail;
        }

        if(need > 0) {
          TSIOBufferCopy(mtc->output.buffer, mtc->res_reader, need, 0);
          TSIOBufferReaderConsume(mtc->res_reader, need);
          mtc->total += need;
          write_down = true;
          mtc->start_pos += need;
        }

      } else {
        avail = TSIOBufferReaderAvail(mtc->res_reader);
        TSIOBufferReaderConsume(mtc->res_reader, avail);
      }
    } else {
      if (mtc->start_pos >= mtc->start_tail) {
        avail = TSIOBufferReaderAvail(mtc->res_reader);

        if (avail > 0) {
          TSIOBufferCopy(mtc->output.buffer, mtc->res_reader, avail, 0);
          TSIOBufferReaderConsume(mtc->res_reader, avail);

          mtc->start_pos += avail;
          mtc->total += avail;
          write_down = true;
        }
      }

    }

  }

trans:

  if (write_down) {//有数据写入
    TSVIOReenable(mtc->output.vio);
  }

  if (toread > 0) {
    TSContCall(TSVIOContGet(input_vio), TS_EVENT_VCONN_WRITE_READY, input_vio);

  } else {//整个流程结束
    TSDebug(PLUGIN_NAME, "last Done Get=%ld, input_vio Done=%ld, mtc->total=%ld", TSVIONDoneGet(mtc->output.vio),
            TSVIONDoneGet(input_vio), mtc->total);
    TSVIONBytesSet(mtc->output.vio, mtc->total);
    TSContCall(TSVIOContGet(input_vio), TS_EVENT_VCONN_WRITE_COMPLETE, input_vio);
  }

  return 1;
}

static int
mp4_parse_meta(Mp4TransformContext *mtc, bool body_complete)
{
  int ret;
  int64_t avail, bytes;
  TSIOBufferBlock blk;
  const char *data;
  Mp4Meta *mm;

  mm = &mtc->mm;

  avail = TSIOBufferReaderAvail(mtc->dup_reader);
  blk   = TSIOBufferReaderStart(mtc->dup_reader);

  while (blk != nullptr) {
    data = TSIOBufferBlockReadStart(blk, mtc->dup_reader, &bytes);
    if (bytes > 0) {
      TSIOBufferWrite(mm->meta_buffer, data, bytes);
    }

    blk = TSIOBufferBlockNext(blk);
  }

  TSIOBufferReaderConsume(mtc->dup_reader, avail);

  ret = mm->parse_meta(body_complete);

  if (ret > 0) { // meta success
    mtc->start_tail           = mm->start_pos;
    mtc->end_tail       = mm->end_pos;
    mtc->content_length = mm->content_length;
    mtc->meta_length    = TSIOBufferReaderAvail(mm->out_handle.reader);
  }

  if (ret != 0) {
    TSIOBufferReaderFree(mtc->dup_reader);
    mtc->dup_reader = nullptr;
  }

  return ret;
}

