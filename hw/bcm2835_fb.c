/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 * This code is licensed under the GNU GPLv2 and later.
 */

// Heavily based on milkymist-vgafb.c, copyright terms below.

/*
 *  QEMU model of the Milkymist VGA framebuffer.
 *
 *  Copyright (c) 2010-2012 Michael Walle <michael@walle.cc>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Specification available at:
 *   http://www.milkymist.org/socdoc/vgafb.pdf
 */

#include "sysbus.h"
#include "qemu-common.h"
#include "qdev.h"
#include "ui/console.h"
#include "framebuffer.h"
#include "ui/pixel_ops.h"

#include "exec/cpu-common.h"

#include "bcm2835_common.h"

// #define LOG_REG_ACCESS

#define FRAMESKIP 1

//#define BITS 8
//#include "milkymist-vgafb_template.h"
//#define BITS 15
//#include "milkymist-vgafb_template.h"
//#define BITS 16
//#include "milkymist-vgafb_template.h"
//#define BITS 24
//#include "milkymist-vgafb_template.h"
//#define BITS 32
//#include "milkymist-vgafb_template.h"

bcm2835_fb_type bcm2835_fb;

typedef struct {
    SysBusDevice busdev;
    MemoryRegion iomem;

    int pending;
    qemu_irq mbox_irq;
} bcm2835_fb_state;

static void fb_invalidate_display(void *opaque)
{
    // bcm2835_fb_state *s = (bcm2835_fb_state *)opaque;
    bcm2835_fb.invalidate = 1;
}

static void draw_line_src16(void *opaque, uint8_t *d, const uint8_t *s,
        int width, int deststep)
{
    uint16_t rgb565;
    uint32_t rgb888;
    uint8_t r, g, b;
    DisplaySurface *surface = qemu_console_surface(bcm2835_fb.con);
    int bpp = surface_bytes_per_pixel(surface) * 8;

    while (width--) {
        switch(bcm2835_fb.bpp) {
        case 8:
            rgb888 = ldl_phys( bcm2835_vcram_base + (*s << 2) );
            r = (rgb888 >> 0) & 0xff;
            g = (rgb888 >> 8) & 0xff;
            b = (rgb888 >> 16) & 0xff;            
            s++;
            break;
        case 16:
            rgb565 = lduw_raw(s);
            r = ((rgb565 >> 11) & 0x1f) << 3;
            g = ((rgb565 >>  5) & 0x3f) << 2;
            b = ((rgb565 >>  0) & 0x1f) << 3;
            s += 2;
            break;
        case 32:
            rgb888 = ldl_raw(s);
            r = (rgb888 >> 0) & 0xff;
            g = (rgb888 >> 8) & 0xff;
            b = (rgb888 >> 16) & 0xff;
            s += 4;
            break;
        default:
            r = 0;
            g = 0;
            b = 0;
            break;
        }
        
        switch(bpp) {
        case 8:
            *d++ = rgb_to_pixel8(r, g, b);
            break;
        case 15:
            *(uint16_t *)d = rgb_to_pixel15(r, g, b);
            d += 2;
            break;
        case 16:
            *(uint16_t *)d = rgb_to_pixel16(r, g, b);
            d += 2;
            break;
        case 24:
            rgb888 = rgb_to_pixel24(r, g, b);
            *d++ = rgb888 & 0xff;
            *d++ = (rgb888 >> 8) & 0xff;
            *d++ = (rgb888 >> 16) & 0xff;
            break;
        case 32:
            *(uint32_t *)d = rgb_to_pixel32(r, g, b);
            d += 4;
            break;
        default:
            return;
        }
    }
}

static void fb_update_display(void *opaque)
{
    bcm2835_fb_state *s = (bcm2835_fb_state *)opaque;
    int first = 0;
    int last = 0;
    drawfn fn;

    int src_width = 0;
    int dest_width = 0;
    
    static uint32_t frame = 0;

    DisplaySurface *surface = qemu_console_surface(bcm2835_fb.con);
    
    if (++frame < FRAMESKIP) {
        return;
    } else {
        frame = 0;
    }
    
    if (bcm2835_fb.lock)
        return;
    
    if (!bcm2835_fb.xres)
        return;
    
    src_width = bcm2835_fb.xres * (bcm2835_fb.bpp >> 3);
    
    dest_width = bcm2835_fb.xres;
    switch (surface_bytes_per_pixel(surface) * 8) {
    case 0:
        return;
    case 8:
        break;
    case 15:
        dest_width *= 2;
        break;
    case 16:
        dest_width *= 2;
        break;
    case 24:
        dest_width *= 3;
        break;
    case 32:
        dest_width *= 4;
        break;
    default:
        hw_error("bcm2835_vgafb: bad color depth\n");
        break;
    }


    
    fn = draw_line_src16;

    framebuffer_update_display(surface,
        sysbus_address_space(&s->busdev),
        bcm2835_fb.base,
        bcm2835_fb.xres,
        bcm2835_fb.yres,
        src_width,
        dest_width,
        0,
        bcm2835_fb.invalidate,
        fn,
        NULL,
        &first, &last);
    if (first >= 0) {
        dpy_gfx_update(bcm2835_fb.con, 0, first, 
            bcm2835_fb.xres, last - first + 1);
    }

    bcm2835_fb.invalidate = 0;
}



static void bcm2835_fb_mbox_push(bcm2835_fb_state *s, uint32_t value) 
{
    value &= ~0xf;
    bcm2835_fb.lock = 1;
    
    bcm2835_fb.xres = ldl_phys(value);
    bcm2835_fb.yres = ldl_phys(value + 4);
    bcm2835_fb.xres_virtual = ldl_phys(value + 8);
    bcm2835_fb.yres_virtual = ldl_phys(value + 12);
    
    bcm2835_fb.bpp = ldl_phys(value + 20);
    bcm2835_fb.xoffset = ldl_phys(value + 24);
    bcm2835_fb.yoffset = ldl_phys(value + 28);
    
    bcm2835_fb.base = bcm2835_vcram_base | (value & 0xc0000000);
    bcm2835_fb.base += BCM2835_FB_OFFSET;
    
    // TODO - Manage properly virtual resolution

    bcm2835_fb.pitch = bcm2835_fb.xres * (bcm2835_fb.bpp >> 3);
    bcm2835_fb.size = bcm2835_fb.yres * bcm2835_fb.pitch;
    
    stl_phys(value + 16, bcm2835_fb.pitch);
    stl_phys(value + 32, bcm2835_fb.base);
    stl_phys(value + 36, bcm2835_fb.size);

#ifdef LOG_REG_ACCESS
    printf("[QEMU] bcm2835_fb: mbox_push res=(%d %d)\n", 
        bcm2835_fb.xres, bcm2835_fb.yres);
#endif

    bcm2835_fb.invalidate = 1;        
    qemu_console_resize(bcm2835_fb.con, bcm2835_fb.xres, bcm2835_fb.yres);
    bcm2835_fb.lock = 0;
}

static uint64_t bcm2835_fb_read(void *opaque, hwaddr offset,
    unsigned size)
{
    bcm2835_fb_state *s = (bcm2835_fb_state *)opaque;
    uint32_t res = 0;
    
    switch(offset) {
    case 0:
        res = MBOX_CHAN_FB;
        s->pending = 0;
        qemu_set_irq(s->mbox_irq, 0);
        break;
    case 4:
        res = s->pending;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
            "bcm2835_fb_read: Bad offset %x\n", (int)offset);
        return 0;        
    }
    return res;
}
static void bcm2835_fb_write(void *opaque, hwaddr offset,
    uint64_t value, unsigned size)
{
    bcm2835_fb_state *s = (bcm2835_fb_state *)opaque;
    switch(offset) {
    case 0:
        if (!s->pending) {
            s->pending = 1;
            bcm2835_fb_mbox_push(s, value);
            qemu_set_irq(s->mbox_irq, 1);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
            "bcm2835_fb_write: Bad offset %x\n", (int)offset);
        return;        
    }    
}


static const MemoryRegionOps bcm2835_fb_ops = {
    .read = bcm2835_fb_read,
    .write = bcm2835_fb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_bcm2835_fb = {
    .name = "bcm2835_fb",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField[]) {
        VMSTATE_END_OF_LIST()
    }
};

static int bcm2835_fb_init(SysBusDevice *dev)
{
    bcm2835_fb_state *s = FROM_SYSBUS(bcm2835_fb_state, dev);
    
    s->pending = 0;
    
    // bcm2835_fb.invalidate = 0;
    bcm2835_fb.xres = 640;
    bcm2835_fb.yres = 480;
    bcm2835_fb.xres_virtual = 640;
    bcm2835_fb.yres_virtual = 480;
    // bcm2835_fb.xres = 1024;
    // bcm2835_fb.yres = 768;
    // bcm2835_fb.xres_virtual = 1024;
    // bcm2835_fb.yres_virtual = 768;
    
    bcm2835_fb.bpp = 16;
    bcm2835_fb.xoffset = 0;
    bcm2835_fb.yoffset = 0;
    
    bcm2835_fb.base = bcm2835_vcram_base;
    bcm2835_fb.base += BCM2835_FB_OFFSET;
    
    bcm2835_fb.pitch = bcm2835_fb.xres * (bcm2835_fb.bpp >> 3);
    bcm2835_fb.size = bcm2835_fb.yres * bcm2835_fb.pitch;

    bcm2835_fb.invalidate = 1;
    bcm2835_fb.lock = 1;
    
    sysbus_init_irq(dev, &s->mbox_irq);
    
    bcm2835_fb.con = graphic_console_init(fb_update_display,
        fb_invalidate_display,
        NULL, NULL, s);
    bcm2835_fb.lock = 0;

    memory_region_init_io(&s->iomem, &bcm2835_fb_ops, s, "bcm2835_fb", 0x10);
    sysbus_init_mmio(dev, &s->iomem);
    vmstate_register(&dev->qdev, -1, &vmstate_bcm2835_fb, s);

    return 0;
}

static void bcm2835_fb_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *sdc = SYS_BUS_DEVICE_CLASS(klass);

    sdc->init = bcm2835_fb_init;
}

static TypeInfo bcm2835_fb_info = {
    .name          = "bcm2835_fb",
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(bcm2835_fb_state),
    .class_init    = bcm2835_fb_class_init,
};

static void bcm2835_fb_register_types(void)
{
    type_register_static(&bcm2835_fb_info);
}

type_init(bcm2835_fb_register_types)
