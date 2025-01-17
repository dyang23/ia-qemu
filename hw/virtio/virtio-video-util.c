/*
 * Virtio Video Device
 *
 * Copyright (C) 2021, Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 *
 * Authors: Colin Xu <colin.xu@intel.com>
 *          Zhuocheng Ding <zhuocheng.ding@intel.com>
 */
#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu/error-report.h"
#include "sysemu/dma.h"
#include "virtio-video-util.h"

//#define VIRTIO_VIDEO_UTIL_DEBUG 1
#if !defined VIRTIO_VIDEO_UTIL_DEBUG && !defined DEBUG_VIRTIO_VIDEO_ALL
#undef DPRINTF
#define DPRINTF(fmt, ...) do { } while (0)
#endif

struct virtio_video_cmd_bh_arg {
    VirtIOVideo *v;
    VirtIOVideoCmd cmd;
    uint32_t stream_id;
};

struct virtio_video_work_bh_arg {
    VirtIOVideo *v;
    VirtIOVideoWork *work;
    uint32_t stream_id;
    uint32_t resource_id;
};

struct virtio_video_event_bh_arg {
    VirtIODevice *vdev;
    uint32_t event_type;
    uint32_t stream_id;
};

static struct {
    virtio_video_cmd_type cmd;
    const char *name;
} virtio_video_cmds[] = {
    { VIRTIO_VIDEO_CMD_QUERY_CAPABILITY, "QUERY_CAPABILITY" },
    { VIRTIO_VIDEO_CMD_STREAM_CREATE, "STREAM_CREATE" },
    { VIRTIO_VIDEO_CMD_STREAM_DESTROY, "STREAM_DESTROY" },
    { VIRTIO_VIDEO_CMD_STREAM_DRAIN, "STREAM_DRAIN" },
    { VIRTIO_VIDEO_CMD_RESOURCE_CREATE, "RESOURCE_CREATE" },
    { VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL, "RESOURCE_DESTROY_ALL" },
    { VIRTIO_VIDEO_CMD_RESOURCE_QUEUE, "RESOURCE_QUEUE" },
    { VIRTIO_VIDEO_CMD_QUEUE_CLEAR, "QUEUE_CLEAR" },
    { VIRTIO_VIDEO_CMD_GET_PARAMS, "GET_PARAMS" },
    { VIRTIO_VIDEO_CMD_SET_PARAMS, "SET_PARAMS" },
    { VIRTIO_VIDEO_CMD_QUERY_CONTROL, "QUERY_CONTROL" },
    { VIRTIO_VIDEO_CMD_GET_CONTROL, "GET_CONTROL" },
    { VIRTIO_VIDEO_CMD_SET_CONTROL, "SET_CONTROL" },
};

static struct {
    virtio_video_format format;
    const char *name;
} virtio_video_formats[] = {
    { VIRTIO_VIDEO_FORMAT_ARGB8888, "ARGB8" },
    { VIRTIO_VIDEO_FORMAT_BGRA8888, "BGRA8" },
    { VIRTIO_VIDEO_FORMAT_NV12, "NV12" },
    { VIRTIO_VIDEO_FORMAT_YUV420, "YUV420(IYUV)" },
    { VIRTIO_VIDEO_FORMAT_YVU420, "YVU420(YV12)" },

    { VIRTIO_VIDEO_FORMAT_MPEG2, "MPEG-2" },
    { VIRTIO_VIDEO_FORMAT_MPEG4, "MPEG-4" },
    { VIRTIO_VIDEO_FORMAT_H264, "H.264(AVC)" },
    { VIRTIO_VIDEO_FORMAT_HEVC, "H.265(HEVC)" },
    { VIRTIO_VIDEO_FORMAT_VP8, "VP8" },
    { VIRTIO_VIDEO_FORMAT_VP9, "VP9" },
};

static struct {
    virtio_video_buffer_flag flag;
    const char *name;
} virtio_video_frame_types[] = {
    {VIRTIO_VIDEO_BUFFER_FLAG_IFRAME, "I-Frame"},
    {VIRTIO_VIDEO_BUFFER_FLAG_PFRAME, "P-Frame"},
    {VIRTIO_VIDEO_BUFFER_FLAG_BFRAME, "B-Frame"},
};

const char *virtio_video_cmd_name(uint32_t cmd)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(virtio_video_cmds); i++) {
        if (virtio_video_cmds[i].cmd == cmd) {
            return virtio_video_cmds[i].name;
        }
    }
    return "UNKNOWN_CMD";
}

const char *virtio_video_format_name(uint32_t format)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(virtio_video_formats); i++) {
        if (virtio_video_formats[i].format == format) {
            return virtio_video_formats[i].name;
        }
    }
    return "UNKNOWN_FORMAT";
}

const char *virtio_video_frame_type_name(uint32_t frame_type)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(virtio_video_frame_types); i++) {
        if (virtio_video_frame_types[i].flag == frame_type) {
            return virtio_video_frame_types[i].name;
        }
    }
    return "UNKNOWN_FRAME_TYPE";
}

int virtio_video_format_profile_range(uint32_t format, uint32_t *min,
                                      uint32_t *max)
{
    if (min == NULL || max == NULL) {
        return -1;
    }

    switch (format) {
    case VIRTIO_VIDEO_FORMAT_H264:
        *min = VIRTIO_VIDEO_PROFILE_H264_MIN;
        *max = VIRTIO_VIDEO_PROFILE_H264_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_HEVC:
        *min = VIRTIO_VIDEO_PROFILE_HEVC_MIN;
        *max = VIRTIO_VIDEO_PROFILE_HEVC_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_VP8:
        *min = VIRTIO_VIDEO_PROFILE_VP8_MIN;
        *max = VIRTIO_VIDEO_PROFILE_VP8_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_VP9:
        *min = VIRTIO_VIDEO_PROFILE_VP9_MIN;
        *max = VIRTIO_VIDEO_PROFILE_VP9_MAX;
        break;
    default:
        return -1;
    }

    return 0;
}

int virtio_video_format_level_range(uint32_t format, uint32_t *min,
                                    uint32_t *max)
{
    if (min == NULL || max == NULL) {
        return -1;
    }

    switch (format) {
    case VIRTIO_VIDEO_FORMAT_H264:
        *min = VIRTIO_VIDEO_LEVEL_H264_MIN;
        *max = VIRTIO_VIDEO_LEVEL_H264_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_HEVC:
        *min = VIRTIO_VIDEO_LEVEL_HEVC_MIN;
        *max = VIRTIO_VIDEO_LEVEL_HEVC_MAX;
        break;
    default:
        return -1;
    }

    return 0;
}

bool virtio_video_format_is_codec(uint32_t format)
{
    switch (format) {
    case VIRTIO_VIDEO_FORMAT_ARGB8888:
    case VIRTIO_VIDEO_FORMAT_BGRA8888:
    case VIRTIO_VIDEO_FORMAT_NV12:
    case VIRTIO_VIDEO_FORMAT_YUV420:
    case VIRTIO_VIDEO_FORMAT_YVU420:
        return false;
    case VIRTIO_VIDEO_FORMAT_MPEG2:
    case VIRTIO_VIDEO_FORMAT_MPEG4:
    case VIRTIO_VIDEO_FORMAT_H264:
    case VIRTIO_VIDEO_FORMAT_HEVC:
    case VIRTIO_VIDEO_FORMAT_VP8:
    case VIRTIO_VIDEO_FORMAT_VP9:
        return true;
    default:
        return false;
    }
}

bool virtio_video_format_is_valid(uint32_t format, uint32_t num_planes)
{
    switch (format) {
    case VIRTIO_VIDEO_FORMAT_ARGB8888:
    case VIRTIO_VIDEO_FORMAT_BGRA8888:
        return num_planes == 1;
    case VIRTIO_VIDEO_FORMAT_NV12:
        return num_planes == 2;
    case VIRTIO_VIDEO_FORMAT_YUV420:
    case VIRTIO_VIDEO_FORMAT_YVU420:
        return num_planes == 3;
    case VIRTIO_VIDEO_FORMAT_MPEG2:
    case VIRTIO_VIDEO_FORMAT_MPEG4:
    case VIRTIO_VIDEO_FORMAT_H264:
    case VIRTIO_VIDEO_FORMAT_HEVC:
    case VIRTIO_VIDEO_FORMAT_VP8:
    case VIRTIO_VIDEO_FORMAT_VP9:
        /* multiplane for bitstream is undefined */
        return num_planes == 1;
    default:
        return false;
    }
}

bool virtio_video_param_fixup(virtio_video_params *params)
{
    switch (params->format) {
    case VIRTIO_VIDEO_FORMAT_ARGB8888:
    case VIRTIO_VIDEO_FORMAT_BGRA8888:
        if (params->num_planes == 1)
            break;
        params->num_planes = 1;
        params->plane_formats[0].plane_size =
            params->frame_width * params->frame_height * 4;
        params->plane_formats[0].stride = params->frame_width * 4;
        return true;
    case VIRTIO_VIDEO_FORMAT_NV12:
        if (params->num_planes == 2)
            break;
        params->num_planes = 2;
        params->plane_formats[0].plane_size =
            params->frame_width * params->frame_height;
        params->plane_formats[0].stride = params->frame_width;
        params->plane_formats[1].plane_size =
            params->frame_width * params->frame_height / 2;
        params->plane_formats[1].stride = params->frame_width;
        return true;
    case VIRTIO_VIDEO_FORMAT_YUV420:
    case VIRTIO_VIDEO_FORMAT_YVU420:
        if (params->num_planes == 3)
            break;
        params->num_planes = 3;
        params->plane_formats[0].plane_size =
            params->frame_width * params->frame_height;
        params->plane_formats[0].stride = params->frame_width;
        params->plane_formats[1].plane_size =
            params->frame_width * params->frame_height / 4;
        params->plane_formats[1].stride = params->frame_width / 2;
        params->plane_formats[2].plane_size =
            params->frame_width * params->frame_height / 4;
        params->plane_formats[2].stride = params->frame_width / 2;
        return true;
    case VIRTIO_VIDEO_FORMAT_MPEG2:
    case VIRTIO_VIDEO_FORMAT_MPEG4:
    case VIRTIO_VIDEO_FORMAT_H264:
    case VIRTIO_VIDEO_FORMAT_HEVC:
    case VIRTIO_VIDEO_FORMAT_VP8:
    case VIRTIO_VIDEO_FORMAT_VP9:
        /* multiplane for bitstream is undefined */
        if (params->num_planes == 1)
            break;
        params->num_planes = 1;
        return true;
    default:
        break;
    }

    return false;
}

void virtio_video_init_format(VirtIOVideoFormat *fmt, uint32_t format)
{
    if (fmt == NULL) {
        return;
    }

    QLIST_INIT(&fmt->frames);
    fmt->desc.mask = 0;
    fmt->desc.format = format;
    fmt->desc.planes_layout = VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER |
                              VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE;
    fmt->desc.plane_align = 0;
    fmt->desc.num_frames = 0;

    fmt->profile.num = 0;
    fmt->profile.values = NULL;
    fmt->level.num = 0;
    fmt->level.values = NULL;
}

void virtio_video_destroy_resource(VirtIOVideoResource *resource,
                                   uint32_t mem_type, bool in)
{
    VirtIOVideoResourceSlice *slice;
    DMADirection dir = in ? DMA_DIRECTION_TO_DEVICE : DMA_DIRECTION_FROM_DEVICE;
    int i, j;

    if (resource->remapped_base)
    {
        munmap(resource->remapped_base, resource->remapped_size);
        resource->remapped_base = NULL;
        resource->remapped_size = 0;
    }
    QLIST_REMOVE(resource, next);
    for (i = 0; i < resource->num_planes; i++)
    {
        for (j = 0; j < resource->num_entries[i]; j++)
        {
            slice = &resource->slices[i][j];
            if (mem_type == VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES)
            {
                dma_memory_unmap(resource->dma_as, slice->page.base,
                                 slice->page.len, dir, slice->page.len);
            } /* TODO: support object memory type */
        }
        g_free(resource->slices[i]);
    }
    g_free(resource);
}

void virtio_video_destroy_resource_list(VirtIOVideoStream *stream, bool in)
{
    VirtIOVideoResource *res, *tmp_res;
    virtio_video_mem_type mem_type;
    int dir;

    if (in) {
        mem_type = stream->in.mem_type;
        dir = VIRTIO_VIDEO_QUEUE_INPUT;
    } else {
        mem_type = stream->out.mem_type;
        dir = VIRTIO_VIDEO_QUEUE_OUTPUT;
    }

    QLIST_FOREACH_SAFE(res, &stream->resource_list[dir], next, tmp_res)
    {
        virtio_video_destroy_resource(res, mem_type, in);
    }
}

// Added by Shenlin 2022.2.28
static int virtio_video_memcpy_singlebuffer_r(VirtIOVideoResource *pRes, 
    uint32_t idx, void *pDst, uint32_t size)
{
    if (pRes == NULL || pDst == NULL)
        return -1;

    VirtIOVideoResourceSlice *pSlice = NULL;
    uint32_t begin = pRes->plane_offsets[idx];
    uint32_t i = 0, cur_len = 0, pos = 0;
    uint32_t diff = 0;

    for ( ; i < pRes->num_entries[0]; i++, pos += pSlice->page.len) {
        pSlice = &pRes->slices[0][i];
        if (begin >= pos + pSlice->page.len) {
            continue;
        }
        diff = begin - pos;
        cur_len = size > pSlice->page.len - diff ? pSlice->page.len - diff : size;
        MEMCPY_S(pDst, pSlice->page.base + diff, cur_len, cur_len);

        pDst += cur_len;
        size -= cur_len;
        begin += cur_len;
    }

    return 0;
}

// Added by Shenlin 2022.2.28
static int virtio_video_memcpy_perplane_r(VirtIOVideoResource *res, 
    uint32_t idx, void *dst, uint32_t size)
{
    VirtIOVideoResourceSlice *slice;
    int i = 0;
    uint32_t dst_pos = 0, cur_len = 0;;

    for (; i < res->num_entries[idx]; i++) {
        slice = &res->slices[idx][i];
        cur_len = size >= slice->page.len ? slice->page.len : size;
        
        MEMCPY_S(dst + dst_pos, slice->page.base, cur_len, cur_len);
        size -= cur_len;
        dst_pos += cur_len;
    }

    if (size > 0) {
        error_report("CMD_RESOURCE_QUEUE: output buffer insufficient "
                     "to contain the frame");
        return -1;
    }

    return 0;
}

static int virtio_video_memcpy_singlebuffer(VirtIOVideoResource *res,
                                            uint32_t idx, void *src,
                                            uint32_t size)
{
    VirtIOVideoResourceSlice *slice;
    uint32_t begin = res->plane_offsets[idx], end = begin + size;
    uint32_t base = 0, diff, len;
    int i;
    DPRINTF("src:%p, size:%d\n", src, (int)size);

    if (res->remapped_base)
    {
        memcpy(res->remapped_base, src, size);
        size=0;
    }
    else
    {
        for (i = 0; i < res->num_entries[0]; i++, base += slice->page.len)
        {
            slice = &res->slices[0][i];
            if (begin >= base + slice->page.len)
                continue;
            /* begin >= base is always true */
            diff = begin - base;
            len = slice->page.len - diff;
            if (end <= base + slice->page.len)
            {
                MEMCPY_S(slice->page.base + diff, src, size, size);
                return 0;
            }
            else
            {
                MEMCPY_S(slice->page.base + diff, src, len, len);
                begin += len;
                size -= len;
                src += len;
            }
        }
    }
    if (size > 0) {
        error_report("CMD_RESOURCE_QUEUE: output buffer insufficient "
                     "to contain the frame");
        return -1;
    }

    return 0;
}


int virtio_video_memcpy_NV12_byline(VirtIOVideoResource *res, void *Y, void *UV,
               uint32_t width, uint32_t height, uint32_t pitch)
{
    uint32_t cp_size = width * height * 3 / 2;
    uint32_t cp_height = height * 3 / 2;

    return virtio_video_memcpy_byline(res, 0, Y, UV, width, height, pitch, cp_size,
		   cp_height);
}

int virtio_video_memcpy_ARGB_byline(VirtIOVideoResource *res, void *src, uint32_t width,
	       uint32_t height, uint32_t pitch)
{
    uint32_t cp_size = width * height * 4;
    uint32_t cp_height = height;

    return virtio_video_memcpy_byline(res, 0, src, src, width * 4, height, pitch, cp_size,
                   cp_height);
}

int virtio_video_memcpy_byline(VirtIOVideoResource *res, uint32_t idx, void *src_begin,
	       void *src_uv, uint32_t width, uint32_t height, uint32_t pitch,
	       uint32_t cp_size, uint32_t cp_height)
{
    VirtIOVideoResourceSlice *slice;
    uint32_t begin = res->plane_offsets[idx];
    uint32_t base = 0, diff, len, size, margin = 0;
    void *src;
    int i, page = 0;

    src = src_begin;
    size = cp_size;
    slice = &res->slices[idx][0];
    diff = begin - base;
    len = slice->page.len - diff;

    for(i = 0; i < cp_height && page < res->num_entries[0]; i++) {
	if (i == height) {
            src = src_uv;
	}
        if (width <= len) {
            MEMCPY_S(slice->page.base + diff, src, width, width);
            src += pitch;
            diff += width;
            size -= width;
            len -= width;
        } else {
            MEMCPY_S(slice->page.base + diff, src, len, len);
            size -= len;
            margin = width - len;
            src += len;
            diff += len;

            //copy to next slice
            while (margin > 0 && page < res->num_entries[0]) {
                page++;
                slice = &res->slices[idx][page];
                len = slice->page.len;
                if (margin <= len) {
                    MEMCPY_S(slice->page.base, src, margin, margin);
                    diff = margin;
                    len -= margin;
                    src += (pitch - width + margin);
                    size -= margin;
                    margin = 0;
                } else {
                    MEMCPY_S(slice->page.base, src, len, len);
		    src += len;
		    size -= len;
		    diff = 0;
		    margin -= len;
		}
	    }
        }
    }

    if (size > 0) {
        error_report("CMD_RESOURCE_QUEUE: output buffer insufficient "
                     "to contain the frame");
        return -1;
    }

    return 0;
}

/*
 * For NV12, the target data will be Y+UV in one plane(contigeous), but the
 * source maybe Y + Blank block + UV（non-contigeous), so need copy seperately
 */
int virtio_video_memcpy_NV12(VirtIOVideoResource *res, void *Y, uint32_t size_Y,
                             void *UV, uint32_t size_UV)
{
    VirtIOVideoResourceSlice *slice;
    uint32_t begin = res->plane_offsets[0], end = begin + size_Y + size_UV;
    uint32_t base = 0, diff, len, size;
    void *src;
    int i;

    src = Y;
    size = size_Y;
    for (i = 0; i < res->num_entries[0]; i++, base += slice->page.len) {
        slice = &res->slices[0][i];
        if (begin >= base + slice->page.len)
            continue;
        /* begin >= base is always true */
        diff = begin - base;
        len = slice->page.len - diff;
        if (end <= base + slice->page.len) {
            MEMCPY_S(slice->page.base + diff, src, size, size);
            return 0;
        } else {
            if (size >= len) {
                MEMCPY_S(slice->page.base + diff, src, len, len);
                begin += len;
                size -= len;
                src += len;
                len = 0;
            } else {
                MEMCPY_S(slice->page.base + diff, src, size, size);
                begin += size;
                src += size;
                len -= size;
                diff += size;
                size = 0;
            }
            if (size == 0 && src <= UV) {
                src = UV;
                size = size_UV;
                MEMCPY_S(slice->page.base + diff, src, len, len);
                begin += len;
                size -= len;
                src += len;
            }
        }
    }

    if (size > 0) {
        error_report("CMD_RESOURCE_QUEUE: output buffer insufficient "
                     "to contain the frame");
        return -1;
    }

    return 0;
}

static int virtio_video_memdump_singlebuffer(VirtIOVideoResource *res,
                                             uint32_t idx, void *dst,
                                             uint32_t size)
{
    VirtIOVideoResourceSlice *slice;
    uint32_t begin = res->plane_offsets[idx], end = begin + size;
    uint32_t base = 0, diff, len;
    int i;

    for (i = 0; i < res->num_entries[0]; i++, base += slice->page.len) {
        slice = &res->slices[0][i];
        if (begin >= base + slice->page.len)
            continue;
        /* begin >= base is always true */
        diff = begin - base;
        len = slice->page.len - diff;
        if (end <= base + slice->page.len) {
            MEMCPY_S(dst, slice->page.base + diff, size, size);
            return 0;
        } else {
            MEMCPY_S(dst, slice->page.base + diff, len, len);
            begin += len;
            size -= len;
            dst += len;
        }
    }

    if (size > 0) {
        error_report("CMD_RESOURCE_QUEUE: output buffer insufficient "
                     "to contain the frame");
        return -1;
    }

    return 0;
}

static int virtio_video_memcpy_perplane(VirtIOVideoResource *res, uint32_t idx,
                                        void *src, uint32_t size)
{
    VirtIOVideoResourceSlice *slice;
    int i;

    for (i = 0; i < res->num_entries[idx]; i++) {
        slice = &res->slices[idx][i];
        if (size <= slice->page.len) {
            MEMCPY_S(slice->page.base, src, size, size);
            return 0;
        } else {
            MEMCPY_S(slice->page.base, src, slice->page.len, slice->page.len);
            size -= slice->page.len;
            src += slice->page.len;
        }
    }

    if (size > 0) {
        error_report("CMD_RESOURCE_QUEUE: output buffer insufficient "
                     "to contain the frame, idx:%d, left size:%d",
                     idx, size);
        return -1;
    }

    return 0;
}

static int virtio_video_memdump_perplane(VirtIOVideoResource *res, uint32_t idx,
                                         void *dst, uint32_t size)
{
    VirtIOVideoResourceSlice *slice;
    int i;

    for (i = 0; i < res->num_entries[idx]; i++) {
        slice = &res->slices[idx][i];
        if (size <= slice->page.len) {
            MEMCPY_S(dst, slice->page.base, size, size);
            return 0;
        } else {
            MEMCPY_S(dst, slice->page.base, slice->page.len, slice->page.len);
            size -= slice->page.len;
            dst += slice->page.len;
        }
    }

    if (size > 0) {
        error_report("CMD_RESOURCE_QUEUE: output buffer insufficient "
                     "to contain the frame");
        // return -1;
    }

    return 0;
}

int virtio_video_memdump(VirtIOVideoResource *res, uint32_t idx, void *dst,
                         uint32_t size)
{
    switch (res->planes_layout) {
    case VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER:
        return virtio_video_memdump_singlebuffer(res, idx, dst, size);
    case VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE:
        return virtio_video_memdump_perplane(res, idx, dst, size);
    default:
        return -1;
    }
}

int virtio_video_memcpy(VirtIOVideoResource *res, uint32_t idx, void *src,
                        uint32_t size)
{
    switch (res->planes_layout) {
    case VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER:
        return virtio_video_memcpy_singlebuffer(res, idx, src, size);
    case VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE:
        return virtio_video_memcpy_perplane(res, idx, src, size);
    default:
        return -1;
    }
}

int virtio_video_memcpy_r(VirtIOVideoResource *res, uint32_t idx, void *dst, 
    uint32_t size)
{
    switch (res->planes_layout) {
        case VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER :
            return virtio_video_memcpy_singlebuffer_r(res, idx, dst, size);
        case VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE :
            return virtio_video_memcpy_perplane_r(res, idx, dst, size);
        default :
            return -1;
    }
}

#if defined DEBUG_VIRTIO_VIDEO || defined VIRTIO_VIDEO_UTIL_DEBUG || defined DEBUG_VIRTIO_VIDEO_ALL
const char *virtio_video_event_name(uint32_t event)
{
    switch (event) {
    case VIRTIO_VIDEO_EVENT_ERROR:
        return "ERROR";
    case VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED:
        return "DECODER_RESOLUTION_CHANGED";
    default:
        return "UNKNOWN";
    }
}
#endif

/* @event must be removed from @event_queue first */
int virtio_video_event_complete(VirtIODevice *vdev, VirtIOVideoEvent *event)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    virtio_video_event resp = { 0 };

    resp.event_type = event->event_type;
    resp.stream_id = event->stream_id;

    DPRINTF_IOV(
        "%s, iov:%p, iov_cnt:%d, copy size:%d, streamid:%d, event_type:0x%x\n",
        __func__, event->elem->in_sg, event->elem->in_num, (int)sizeof(resp),
        resp.stream_id, resp.event_type);

    if (unlikely(iov_from_buf(event->elem->in_sg, event->elem->in_num, 0, &resp,
                              sizeof(resp)) != sizeof(resp))) {
        virtio_error(vdev, "virtio-video event input incorrect");
        virtqueue_detach_element(v->event_vq, event->elem, 0);
        g_free(event->elem);
        g_free(event);
        return -1;
    }

    virtqueue_push(v->event_vq, event->elem, sizeof(resp));
    virtio_notify(vdev, v->event_vq);

    DPRINTF("stream %d event %s triggered\n", event->stream_id,
            virtio_video_event_name(resp.event_type));
    g_free(event->elem); // event->elem pushed to virtqueue, any race if free it
                         // remove it immediately?
    g_free(event);
    return 0;
}

/*
 * Before the response of CMD_RESOURCE_QUEUE can be sent, these conditions must
 * be met:
 *  @work:           should be removed from @input_work or @output_work
 *  @work->resource: should be removed from @resource_list and destroyed
 */
static int virtio_video_cmd_resource_queue_complete(VirtIOVideo *v,
                                                    VirtIOVideoWork *work,
                                                    uint32_t stream_id,
                                                    uint32_t resource_id)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(v);
    virtio_video_resource_queue_resp resp = { 0 };

    resp.hdr.type = VIRTIO_VIDEO_RESP_OK_NODATA;
    resp.hdr.stream_id = stream_id;
    resp.timestamp = work->timestamp;
    resp.flags = work->flags;
    resp.size = work->size;
    
    DPRINTF("resp.timestamp = work->timestamp = %lu \n",
            work->timestamp / 1000000000);

    DPRINTF("type:%d, streamID:%d, flags:%d, size:%d\n", resp.hdr.type,
            resp.hdr.stream_id, resp.flags, resp.size);

    if (unlikely(iov_from_buf(work->elem->in_sg, work->elem->in_num, 0, &resp,
                              sizeof(resp)) != sizeof(resp))) {
        virtio_error(vdev, "virtio-video command response incorrect");
        virtqueue_detach_element(v->cmd_vq, work->elem, 0);
        g_free(work->elem);
        g_free(work);
        return -1;
    }

    virtqueue_push(v->cmd_vq, work->elem, sizeof(resp));
    virtio_notify(vdev, v->cmd_vq);

    DPRINTF("CMD_RESOURCE_QUEUE complete: stream %d dequeued %s resource %d, "
            "flags=0x%x size=%d\n",
            stream_id,
            work->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT ? "input" :
                                                                "output",
            resource_id, work->flags, work->size);
    g_free(work->elem);
    g_free(work);
    return 0;
}
//#define USE_BH_FOR_OUTPUT
#ifdef USE_BH_FOR_OUTPUT
static void virtio_video_output_one_work_bh(void *opaque)
{
    struct virtio_video_work_bh_arg *s = opaque;

    virtio_video_cmd_resource_queue_complete(s->v, s->work, s->stream_id,
                                             s->resource_id);
    object_unref(OBJECT(s->v));
    g_free(opaque);
}
#endif
/* must be called with stream->mutex held */
void virtio_video_work_done(VirtIOVideoWork *work)
{
#ifdef USE_BH_FOR_OUTPUT
    struct virtio_video_work_bh_arg *s;
    VirtIOVideoStream *stream = work->parent;
    VirtIOVideo *v = stream->parent;

    s = g_new0(struct virtio_video_work_bh_arg, 1);
    s->v = v;
    s->work = work;
    s->stream_id = stream->id;
    s->resource_id = work->resource->id;

    object_ref(OBJECT(v));
    aio_bh_schedule_oneshot(v->ctx, virtio_video_output_one_work_bh, s);
#else
    VirtIOVideoStream *stream = work->parent;
    VirtIOVideo *v = stream->parent;
    virtio_video_cmd_resource_queue_complete(v, work, stream->id,
                                             work->resource->id);
#endif
}

static void virtio_video_cmd_others_complete(struct virtio_video_cmd_bh_arg *s,
                                             bool success)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s->v);
    virtio_video_cmd_hdr resp = { 0 };

    resp.type = success ? VIRTIO_VIDEO_RESP_OK_NODATA :
                          VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp.stream_id = s->stream_id;

    if (unlikely(iov_from_buf(s->cmd.elem->in_sg, s->cmd.elem->in_num, 0, &resp,
                              sizeof(resp)) != sizeof(resp))) {
        virtio_error(vdev, "virtio-video command response incorrect");
        virtqueue_detach_element(s->v->cmd_vq, s->cmd.elem, 0);
        g_free(s->cmd.elem);
        return;
    }

    virtqueue_push(s->v->cmd_vq, s->cmd.elem, sizeof(resp));
    virtio_notify(vdev, s->v->cmd_vq);

    switch (s->cmd.cmd_type) {
    case VIRTIO_VIDEO_CMD_STREAM_DRAIN:
        DPRINTF("CMD_STREAM_DRAIN (async) for stream %d %s\n", s->stream_id,
                success ? "done" : "cancelled");
        break;
    case VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL:
        DPRINTF("CMD_RESOURCE_DESTROY_ALL (async) for stream %d %s\n",
                s->stream_id, success ? "done" : "cancelled");
        break;
    case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
        DPRINTF("CMD_QUEUE_CLEAR (async) for stream %d %s\n", s->stream_id,
                success ? "done" : "cancelled");
        break;
    case VIRTIO_VIDEO_CMD_STREAM_DESTROY:
        DPRINTF("CMD_STREAM_DESTROY (async) for stream %d %s\n", s->stream_id,
                success ? "done" : "cancelled");
        break;
    default:
        break;
    }
    g_free(s->cmd.elem);
    object_unref(OBJECT(s->v));
}

static void virtio_video_cmd_done_bh(void *opaque)
{
    struct virtio_video_cmd_bh_arg *s = opaque;

    virtio_video_cmd_others_complete(s, true);
    g_free(opaque);
}

static void virtio_video_cmd_cancel_bh(void *opaque)
{
    struct virtio_video_cmd_bh_arg *s = opaque;

    virtio_video_cmd_others_complete(s, false);
    g_free(opaque);
}

void virtio_video_inflight_cmd_done(VirtIOVideoStream *stream)
{
    struct virtio_video_cmd_bh_arg *s;
    VirtIOVideo *v = stream->parent;

    s = g_new0(struct virtio_video_cmd_bh_arg, 1);
    s->v = v;
    s->cmd = stream->inflight_cmd;
    s->stream_id = stream->id;
    stream->inflight_cmd.cmd_type = 0;

    object_ref(OBJECT(v));
    aio_bh_schedule_oneshot(v->ctx, virtio_video_cmd_done_bh, s);
}

void virtio_video_inflight_cmd_cancel(VirtIOVideoStream *stream)
{
    struct virtio_video_cmd_bh_arg *s;
    VirtIOVideo *v = stream->parent;

    s = g_new0(struct virtio_video_cmd_bh_arg, 1);
    s->v = v;
    s->cmd = stream->inflight_cmd;
    s->stream_id = stream->id;
    stream->inflight_cmd.cmd_type = 0;

    object_ref(OBJECT(v));
    aio_bh_schedule_oneshot(v->ctx, virtio_video_cmd_cancel_bh, s);
}

static void virtio_video_event_bh(void *opaque)
{
    struct virtio_video_event_bh_arg *s = opaque;
    VirtIODevice *vdev = s->vdev;
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoEvent *event, *tmp_event;
    VirtQueueElement *elem;
    VirtQueue *vq = v->event_vq;

    qemu_mutex_lock(&v->mutex);
    QTAILQ_FOREACH_SAFE(event, &v->event_queue, next, tmp_event)
    {
        DPRINTF_EVENT("event_queue_debug, %s, all event in event_queue:%p\n",
                      __func__, event);
    }

    event = g_new0(VirtIOVideoEvent, 1);
    event->event_type = s->event_type;
    event->stream_id = s->stream_id;

    elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
    if (!elem || elem->in_num < 1 ||
        elem->in_sg[0].iov_len < sizeof(virtio_video_event)) {
        // no usable element in vq, queue event and wait for new element
        QTAILQ_INSERT_TAIL(&v->event_queue, event, next);
        if (elem) {
            virtio_error(vdev, "virtio-video event error\n");
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
        }

        goto done;
    }

    event->elem = elem;
    DPRINTF_EVENT("event_queue_debug, %s, complete event %p\n", __func__,
                  event);
    virtio_video_event_complete(vdev, event);

done:
    qemu_mutex_unlock(&v->mutex);
    object_unref(OBJECT(v));
    g_free(opaque);
}

void virtio_video_report_event(VirtIOVideo *v, uint32_t event,
                               uint32_t stream_id)
{
    struct virtio_video_event_bh_arg *s;

    s = g_new0(struct virtio_video_event_bh_arg, 1);
    s->vdev = VIRTIO_DEVICE(v);
    s->event_type = event;
    s->stream_id = stream_id;

    object_ref(OBJECT(v));
    aio_bh_schedule_oneshot(v->ctx, virtio_video_event_bh, s);
}
