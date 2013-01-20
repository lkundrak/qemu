/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 * This code is licensed under the GNU GPLv2 and later.
 */

#include "sysbus.h"
#include "qemu-common.h"
#include "qdev.h"
#include "hw/usb.h"

#include "bcm2835_usb_regs.h"

#define LOG_REG_ACCESS

#define NB_HCHANS 8

typedef struct {
	uint32_t hcchar;
	uint32_t hcsplt;
	uint32_t hcint;
	uint32_t hcintmsk;
	uint32_t hctsiz;
	uint32_t hcdma;
	uint32_t reserved;
	uint32_t hcdmab;
} bcm2835_usb_hc_state;

typedef struct {
    SysBusDevice busdev;
    MemoryRegion iomem;
    
    USBBus bus;
    USBPort port;
    int attached;
    
    uint32_t gusbcfg;
    uint32_t hptxfsiz;
    uint32_t hcfg;
    uint32_t dcfg;
    uint32_t grxfsiz;
    uint32_t gnptxfsiz;
    uint32_t dtxfsiz[15];
    uint32_t gahbcfg;
    uint32_t grstctl;
    uint32_t gotgctl;
    uint32_t gotgint;
    uint32_t gintsts;
    uint32_t gintmsk;
    uint32_t gdfifocfg;
    uint32_t hprt0;

    bcm2835_usb_hc_state hchan[NB_HCHANS];

    qemu_irq irq;
    
} bcm2835_usb_state;


static void bcm2835_usb_update_irq(bcm2835_usb_state *s) {
    if (!(s->gahbcfg & gahbcfg_glblintrmsk)) {
        qemu_set_irq(s->irq, 0);
    } else {
        printf("[QEMU] bcm2835_usb_update_irq gintsts=%08x gintmsk=%08x\n",
            s->gintsts, s->gintmsk);
        if (s->gintsts & s->gintmsk) {
            qemu_set_irq(s->irq, 1);
        } else {
            qemu_set_irq(s->irq, 0);
        }
    }
}

static uint32_t bcm2835_usb_hchan_read(bcm2835_usb_state *s, int ch,
    int offset) {
    
    bcm2835_usb_hc_state *c = &s->hchan[ch];
    uint32_t res;
    int unmapped = 0;
    
    switch(offset) {
    case 0x0:
        res = c->hcchar;
        break;
    case 0x4:
        res = c->hcsplt;
        break;
    case 0x8:
        res = c->hcint;
        break;
    case 0xc:
        res = c->hcintmsk;
        break;
    case 0x10:
        res = c->hctsiz;
        break;
    case 0x14:
        res = c->hcdma;
        break;
    case 0x1c:
        res = c->hcdmab;
        break;
    default:
        unmapped = 1;
        res = 0;
        break;
    }

#ifdef LOG_REG_ACCESS
    printf("[QEMU] bcm2835_usb: read_hc[%d](%x) %08x %s\n", ch,
        (int)offset, res, (unmapped ? "(unmapped)" : "") );
#endif
    
    return res;
} 
static void bcm2835_usb_hchan_write(bcm2835_usb_state *s, int ch,
    int offset, uint32_t value) {
    
    bcm2835_usb_hc_state *c = &s->hchan[ch];
    int unmapped = 0;
    
    switch(offset) {
    case 0x0:
        c->hcchar = value;
        if (value & hcchar_chdis) {
            c->hcchar &= ~(hcchar_chdis | hcchar_chen);
            // TODO irq
        }
        break;
    case 0x4:
        c->hcsplt = value;
        break;
    case 0x8:
        // Looks like a standard interrupt register
        c->hcint &= ~value;
        break;
    case 0xc:
        c->hcintmsk = value;
        break;
    case 0x10:
        c->hctsiz = value;
        break;
    case 0x14:
        c->hcdma = value;
        break;
    case 0x1c:
        c->hcdmab = value;
        break;
    default:
        unmapped = 1;
        break;
    }

#ifdef LOG_REG_ACCESS
    printf("[QEMU] bcm2835_usb: write_hc[%d](%x) %08x %s\n", ch,
        (int)offset, value, (unmapped ? "(unmapped)" : "") );
#endif
    
} 

static uint64_t bcm2835_usb_read(void *opaque, hwaddr offset,
    unsigned size)
{
    bcm2835_usb_state *s = (bcm2835_usb_state *)opaque;
    uint32_t res = 0;
    int unmapped = 0;
    int log = 1;
    int i;

    assert(size == 4);
    
    switch(offset) {
    case 0x0:   // gotgctl
        res = s->gotgctl;
        break;
    case 0x4:   // gotgint
        res = s->gotgint;
        break;
    case 0x8:   // gahbcfg
        res = s->gahbcfg;
        break;
    case 0xc:   // gusbcfg
        res = s->gusbcfg;
        break;
    case 0x10:  // grstctl
        res = s->grstctl;
        break;
    case 0x14:  // gintsts
        res = s->gintsts;
        // Enforce Host mode
        res |= gintsts_curmode;
        break;
    case 0x18:  // gintmsk
        res = s->gintmsk;
        break;
    case 0x24:  // grxfsiz
        res = s->grxfsiz;
        break;
    case 0x28:  // gnptxfsiz
        res = s->gnptxfsiz;
        break;
    case 0x40:  // gsnpsid
        res = 0x4f54280a;
        break;
    case 0x44:  // ghwcfg1
        res = 0;
        break;
    case 0x48:  // ghwcfg2
        res = 0x228ddd50;
        break;
    case 0x4c:  // ghwcfg3
        res = 0x0ff000e8;
        break;
    case 0x50:  // ghwcfg4
        res = 0x1ff00020;
        break;
    case 0x5c:  // gdfifocfg
        res = s->gdfifocfg;
        break;
    case 0x100: // hptxfsiz
        res = s->hptxfsiz;
        break;    
    case 0x400: // hcfg
        res = s->hcfg;
        break;
    case 0x440: // hprt0
        res = s->hprt0;
        res &= ~hprt0_prtconnsts;
        if (s->attached)
            res |= hprt0_prtconnsts;        
        break;
    case 0x800: // dcfg
        res = s->dcfg;
        break;

    default:
        if ((offset >= 0x104) && (offset < 0x104 + (15 << 2))) {
            // dtxfsiz[0..14]
            res = s->dtxfsiz[(offset - 0x104) >> 2];
        } else if ((offset >= 0x500) && (offset < 0x500 + 0x20*NB_HCHANS)) {
            i = (offset - 0x500) >> 5;
            res = bcm2835_usb_hchan_read(s, i, offset & 0x1f);
            log = 0;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                "bcm2835_usb_read: Bad offset %x\n", (int)offset);
            unmapped = 1;
            res = 0;
        }
        break;
    }

#ifdef LOG_REG_ACCESS
    if (log) printf("[QEMU] bcm2835_usb: read(%x) %08x %s\n", (int)offset, res,
        (unmapped ? "(unmapped)" : "") );
#endif

    return res;
}

static void bcm2835_usb_write(void *opaque, hwaddr offset,
    uint64_t value, unsigned size)
{
    bcm2835_usb_state *s = (bcm2835_usb_state *)opaque;
    int unmapped = 0;
    int log = 1;
    int i;
    int set_irq = 0;
    
    assert(size == 4);
    
    switch(offset) {
    case 0x0:
        s->gotgctl = value;
        break;
    case 0x4:   // gotgint
        // Looks like a standard interrupt register
        s->gotgint &= ~value;
        break;
    case 0x8:   // gahbcfg
        s->gahbcfg = value;
        set_irq = 1;
        break;
    case 0xc:   // gusbcfg
        s->gusbcfg = value;
        break;
    case 0x10:  // grstctl
        s->grstctl &= ~0x7c0;
        s->grstctl |= value & 0x7c0;
        break;
    case 0x14:  // gintsts
        if (value & gintsts_sofintr)
            s->gintsts &= ~gintsts_sofintr;
        set_irq = 1;
        break;
    case 0x18:  // gintmsk
        s->gintmsk = value;
        break;
    case 0x24:  // grxfsiz
        s->grxfsiz = value;
        break;
    case 0x28:  // gnptxfsiz
        s->gnptxfsiz = value;
        break;
    case 0x5c:  // gdfifocfg
        s->gdfifocfg = value;
        break;
    case 0x100: // hptxfsiz
        s->hptxfsiz = value;
        break;
    case 0x400: // hcfg
        s->hcfg = value;
        break;
    case 0x440: // hprt0
        if (!(s->hprt0 & hprt0_prtpwr) && (value & hprt0_prtpwr)) {
            // Trigger the port status change interrupt on power on
            if (s->attached) {
                s->hprt0 |= hprt0_prtconndet;
                set_irq = 1;
            }
        }
        s->hprt0 &= ~hprt0_prtpwr;
        s->hprt0 |= value & hprt0_prtpwr;
        
        if ( (s->hprt0 & hprt0_prtres) ^ (value & hprt0_prtres) ) {
            s->hprt0 |= hprt0_prtenchng;
            set_irq = 1;            
        }
        s->hprt0 &= ~(hprt0_prtena | hprt0_prtres);
        if (value & hprt0_prtres) {
            s->hprt0 |= hprt0_prtres;
        } else {
            s->hprt0 |= hprt0_prtena;
        }
        
        /*s->hprt0 &= ~hprt0_prtenchng;
        if ( (s->hprt0 & hprt0_prtena) ^ (value & hprt0_prtena) ) {
            s->hprt0 |= hprt0_prtenchng;
            set_irq = 1;
        }
        s->hprt0 &= ~hprt0_prtena;
        s->hprt0 |= value & hprt0_prtena;*/

        // Interrupt clears
        if (value & hprt0_prtconndet) {
            s->hprt0 &= ~hprt0_prtconndet;
            set_irq = 1;
        }
        if (value & hprt0_prtenchng) {
            s->hprt0 &= ~hprt0_prtenchng;
            set_irq = 1;
        }

        // Set portintr according to hprt0's irq sources values
        if (set_irq) {
            if ( (s->hprt0 & hprt0_prtconndet)
                || (s->hprt0 & hprt0_prtenchng)
                ) {
                s->gintsts |= gintsts_portintr;
            } else {
                s->gintsts &= ~gintsts_portintr;
            }
        }

        break;
            
    default:
        if ((offset >= 0x104) && (offset < 0x104 + (15 << 2))) {
            // dtxfsiz[0..14]
            s->dtxfsiz[(offset - 0x104) >> 2] = value;
        } else if ((offset >= 0x500) && (offset < 0x500 + 0x20*NB_HCHANS)) {
            i = (offset - 0x500) >> 5;
            bcm2835_usb_hchan_write(s, i, offset & 0x1f, value);
            log = 0;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                "bcm2835_usb_write: Bad offset %x\n", (int)offset);
            unmapped = 1;
        }
        break;        
    }
    
#ifdef LOG_REG_ACCESS
    if (log) printf("[QEMU] bcm2835_usb: write(%x) %08x %s\n", (int)offset, 
        (uint32_t)value, (unmapped ? "(unmapped)" : ""));
#endif

    if (set_irq)
        bcm2835_usb_update_irq(s);
}

static void bcm2835_usb_attach(USBPort *port1) 
{
    printf("[QEMU] port_attach\n");
    bcm2835_usb_state *s = port1->opaque;
    s->attached = 1;
}
static void bcm2835_usb_detach(USBPort *port1) 
{
    printf("******************* DETACH\n");
}
static void bcm2835_usb_child_detach(USBPort *port1, USBDevice *child) 
{
    printf("******************* CHILD DETACH\n");    
}
static void bcm2835_usb_wakeup(USBPort *port1)
{
    printf("******************* WAKEUP\n");
}
static void bcm2835_usb_async_complete(USBPort *port, USBPacket *packet)
{
    printf("******************* ASYNC COMPLETE\n");
}


static const MemoryRegionOps bcm2835_usb_ops = {
    .read = bcm2835_usb_read,
    .write = bcm2835_usb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_bcm2835_usb = {
    .name = "bcm2835_usb",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField[]) {
        VMSTATE_END_OF_LIST()
    }
};

static USBPortOps bcm2835_usb_port_ops = {
    .attach = bcm2835_usb_attach,
    .detach = bcm2835_usb_detach,
    .child_detach = bcm2835_usb_child_detach,
    .wakeup = bcm2835_usb_wakeup,
    .complete = bcm2835_usb_async_complete,
};

static USBBusOps bcm2835_usb_bus_ops = {
};

static int bcm2835_usb_init(SysBusDevice *dev)
{
    bcm2835_usb_state *s = FROM_SYSBUS(bcm2835_usb_state, dev);
    int n;
    
    s->gusbcfg = 0x20402700;
    s->hptxfsiz = 0x02002000;
    s->hcfg = 0x00000001;
    s->dcfg = 0x00000000;
    s->grxfsiz = 0x00001000;
    s->gnptxfsiz = 0x01001000;
    for(n = 0; n < 15; n++) {
        s->dtxfsiz[n] = 0x02002000;
    }
    s->gahbcfg = 0x0000000e;
    s->grstctl = 0x80000000;
    s->gotgctl = 0x001c0000;
    s->gotgint = 0;
    s->gintsts = 0;
    s->gintmsk = 0;
    s->gdfifocfg = 0x00000000;
    // s->hprt0 = 0x00000400;
    s->hprt0 = DWC_HPRT0_PRTSPD_FULL_SPEED << hprt0_prtspd_shift;
    
    for(n = 0; n < NB_HCHANS; n++) {
        s->hchan[n].hcchar = 0;
        s->hchan[n].hcsplt = 0;
        s->hchan[n].hcint = 0;
        s->hchan[n].hcintmsk = 0;
        s->hchan[n].hctsiz = 0;
        s->hchan[n].hcdma = 0;
        s->hchan[n].hcdmab = 0;
    }
    
    memory_region_init_io(&s->iomem, &bcm2835_usb_ops, s, 
        "bcm2835_usb", 0x20000);
    sysbus_init_mmio(dev, &s->iomem);    
    vmstate_register(&dev->qdev, -1, &vmstate_bcm2835_usb, s);

    sysbus_init_irq(dev, &s->irq);

    s->attached = 0;
    usb_bus_new(&s->bus, &bcm2835_usb_bus_ops, &dev->qdev);
    usb_register_port(&s->bus, &s->port, s, 0, &bcm2835_usb_port_ops, 
        USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL );
    return 0;
}

static void bcm2835_usb_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *sdc = SYS_BUS_DEVICE_CLASS(klass);
 
    sdc->init = bcm2835_usb_init;
}

static TypeInfo bcm2835_usb_info = {
    .name          = "bcm2835_usb",
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(bcm2835_usb_state),
    .class_init    = bcm2835_usb_class_init,
};

static void bcm2835_usb_register_types(void)
{
    type_register_static(&bcm2835_usb_info);
}

type_init(bcm2835_usb_register_types)
