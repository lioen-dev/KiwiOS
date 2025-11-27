#include <stdint.h>
#include <stdbool.h>
#include "limine.h"
#include "drivers/hda.h"
#include "drivers/pci.h"
#include "memory/heap.h"
#include "memory/hhdm.h"

// Minimal print shims so we can reuse the upstream driver diagnostics.
extern void print(struct limine_framebuffer* fb, const char* s);
extern void print_hex(struct limine_framebuffer* fb, uint64_t num);
extern struct limine_framebuffer* fb0(void);

// ===== Begin code based on https://github.com/inclementine/intelHDA =====

static pci_device_t g_hda_pci = {0};
static struct hda_device audio_device;

static volatile uint8_t* hda_mmio = NULL;

static uint32_t rings[4096];
static uint8_t kb[8];

#define BDL_SIZE                      4
#define BUFFER_SIZE             0x10000
#define CORBSIZE 0x4E
#define RIRBSIZE 0x5E
#define CORBLBASE 0x40
#define RIRBLBASE 0x50
#define RIRBSTS 0x5d
#define RIRBCTL 0x5C
#define CORBCTL 0x4C
#define INTSTS 0x24
#define INTCTL 0x20
#define BIT_2 0x1 << 1
#define RIRBCTL_RIRBRUN BIT_2
#define CORBCTL_CORBRUN BIT_2
#define CORBWP 0x48
#define GCTL 0x08
#define BIT_1 0x1 << 0
#define CRST BIT_1
#define WAKEEN 0x0C
#define STATESTS 0x0E
#define SDIN_LEN 16
#define CORBRP  0x4a
#define RIRBWP 0x58

/* VERBS! */
//Shifted over byte so it's easier to OR with payload
//skating by with these...
#define VERB_GET_PARAMETER      0xf0000
#define VERB_SET_STREAM_CHANNEL 0x70600
#define VERB_SET_FORMAT         0x20000
#define VERB_GET_AMP_GAIN_MUTE  0xb0000
#define VERB_SET_AMP_GAIN_MUTE  0x30000
#define VERB_GET_CONFIG_DEFAULT 0xf1c00
#define VERB_GET_CONN_LIST      0xf0200
#define VERB_GET_CONN_SELECT    0xf0100
#define VERB_GET_PIN_CONTROL    0xf0700
#define VERB_SET_PIN_CONTROL    0x70700
#define VERB_GET_EAPD_BTL       0xf0c00
#define VERB_SET_EAPD_BTL       0x70c00
#define VERB_GET_POWER_STATE    0xf0500
#define VERB_SET_POWER_STATE    0x70500
/* PARAMS! */
#define PARAM_SUB_NODE_COUNT      0x04
#define PARAM_FUNCTION_GROUP_TYPE       0x05
#define PARAM_WIDGET_CAPABILITIES 0x09
#define PARAM_OUT_AMP_CAP         0x12
#define PARAM_PIN_CAPABILITIES    0x0c

/* WIDGETS! */

#define WIDGET_OUTPUT           0x0
#define WIDGET_INPUT            0x1
#define WIDGET_MIXER            0x2
#define WIDGET_SELECTOR         0x3
#define WIDGET_PIN_COMPLEX      0x4
#define WIDGET_POWER            0x5
#define WIDGET_VOLUME_KNOB      0x6
#define WIDGET_BEEP_GEN         0x7
#define WIDGET_VENDOR_DEFINED   0xF
#define PARAM_CONN_LIST_LEN     0xE

void HDA_set_volume(uint8_t vol);

static inline void hda_log(const char* s) {
    print(fb0(), s);
}

static inline void hda_log_hex(const char* prefix, uint64_t v, const char* suffix) {
    if (prefix) hda_log(prefix);
    print_hex(fb0(), v);
    if (suffix) hda_log(suffix);
}

static inline uint8_t hda_reg_readbyte(uint32_t reg_offset){
    return *((volatile uint8_t*)(hda_mmio + reg_offset));
}

static inline uint16_t hda_reg_readword(uint32_t reg_offset){
    return *((volatile uint16_t*)(hda_mmio + reg_offset));
}

static inline uint32_t hda_reg_readlong(uint32_t reg_offset){
    return *((volatile uint32_t*)(hda_mmio + reg_offset));
}

static inline void hda_reg_writebyte(uint32_t reg_offset, uint8_t val){
    (*((volatile uint8_t*)(hda_mmio + reg_offset)))=(val);
}
static inline void hda_reg_writeword(uint32_t reg_offset, uint16_t val){
    (*((volatile uint16_t*)(hda_mmio + reg_offset)))=(val);
}
static inline void hda_reg_writelong(uint32_t reg_offset, uint32_t val){
    (*((volatile uint32_t*)(hda_mmio + reg_offset)))=(val);
}

void hda_interrupt_handler(){
    //uint32_t int_status = hda_reg_readlong(INTSTS);
    //TODO: THIS!
    (void)rings;
    (void)kb;
}

void HDA_rirb_init(){
    uint8_t reg;
    uint64_t base;
    uint64_t intermediate;
    reg = hda_reg_readbyte(RIRBSIZE);
    if((reg & 0x20) != 0){hda_log("RIRBSIZE = 128B\n");
        reg |= 0x1;
        audio_device.rirb_entries = 16;
        }
    else if((reg & 0x40) != 0){hda_log("RIRBSIZE = 2048B\n");
        reg |= 0x2;
        audio_device.rirb_entries = 256;
        }
    else if((reg & 0x10) != 0){hda_log("RIRBSIZE = 16B\n");
    audio_device.rirb_entries = 2;
    }
    //SET DMA WRAP-AROUND POINT! with these bits! page 44
    hda_reg_writebyte(RIRBSIZE, reg);
    hda_log("RIRBSIZE: ");
    hda_log_hex(NULL, hda_reg_readbyte(RIRBSIZE), "\n");
    void* buf = kmalloc(audio_device.rirb_entries*8*2);
    intermediate = (uint64_t)hhdm_virt_to_phys(buf);
    intermediate += 0xFF;
    intermediate &= ~0x7FULL;
    hda_log("Intermediate 0x");
    hda_log_hex(NULL, intermediate, "\n");
    audio_device.rirb = (uint32_t*)hhdm_phys_to_virt(intermediate);
    base = intermediate;

    hda_reg_writelong(RIRBLBASE, (uint32_t)(base & 0xFFFFFFFFu));
    hda_reg_writelong(RIRBLBASE + 4, (uint32_t)(base >> 32));

    hda_log("RIRBBase address int: 0x");
    hda_log_hex(NULL, base, "\n");
    hda_log("RIRBBase address from read: 0x");
    hda_log_hex(NULL, hda_reg_readlong(RIRBLBASE + 4), "");
    hda_log_hex(NULL, hda_reg_readlong(RIRBLBASE), "\n");
    hda_reg_writeword(0x5A, 0x42);
    //RUN THE DMA ENGINE!!! yeehaw

    hda_reg_writebyte(RIRBCTL, BIT_2);
    hda_log("DMA_ENGINE STATUS: 0x");
    hda_log_hex(NULL, hda_reg_readbyte(RIRBCTL), "\n");

}

void HDA_corb_init(){
    uint8_t reg;
    uint64_t base;
    uint64_t intermediate;
    hda_reg_writeword(0x5A, 0xFF);
    reg = hda_reg_readbyte(CORBSIZE);
    if((reg & 0x20) != 0){hda_log("CORBSIZE = 128B\n");
        reg |= 0x1;
        audio_device.corb_entries = 16;
        }
    else if((reg & 0x40) != 0){hda_log("CORBSIZE = 2048B\n");
        reg |= 0x2;
        audio_device.corb_entries = 256;
        }
    else if((reg & 0x10) != 0){hda_log("CORBSIZE = 16B\n");
    audio_device.corb_entries = 2;
    }
    //SET DMA WRAP-AROUND POINT! with these bits! page 44
    hda_reg_writebyte(CORBSIZE, reg);
    void* buf = kmalloc(audio_device.corb_entries*8*2);
    //Bodge so I don't have to worry about the bottom 6 bits being 0.
    intermediate = (uint64_t)hhdm_virt_to_phys(buf);
    intermediate += 0xFF;
    intermediate &= ~0x7FULL;

    audio_device.corb = (uint32_t*)hhdm_phys_to_virt(intermediate);
    base = intermediate;
    hda_reg_writelong(CORBLBASE, (uint32_t)(base & 0xFFFFFFFFu));
    hda_reg_writelong(CORBLBASE + 4, (uint32_t)(base >> 32));

    hda_log("CORBBase address int: 0x");
    hda_log_hex(NULL, base, "\n");
    hda_log("CORBBase address from read: 0x");
    hda_log_hex(NULL, hda_reg_readlong(CORBLBASE + 4), "");
    hda_log_hex(NULL, hda_reg_readlong(CORBLBASE), "\n");

    //RUN THE DMA ENGINE!!! yeehaw
    hda_reg_writebyte(CORBCTL, BIT_2);
}

void HDA_corb_write(uint32_t verb){
    uint16_t write_pointer;
    uint16_t read_pointer;
    uint16_t next;

    write_pointer = hda_reg_readbyte(CORBWP);
    next = (uint16_t)((write_pointer + 1) % audio_device.corb_entries);

    do{
        read_pointer = (uint16_t)(hda_reg_readword(CORBRP) & 0xFF);
    }while((next == read_pointer));
    audio_device.corb[next] = verb;

    hda_reg_writeword(CORBWP, next);
}

//for symmetry
void HDA_rirb_write(){return;}

//for symmetry
void HDA_corb_read(){return;}

void HDA_rirb_read(uint32_t* response){
    uint16_t write_pointer;
    uint16_t read_pointer;
    uint8_t i;

    read_pointer = audio_device.rirb_read_pointer;
    do{
        write_pointer = (uint16_t)(hda_reg_readword(RIRBWP) & 0xFF);
    }while (write_pointer == read_pointer);

    read_pointer = (uint16_t)((read_pointer + 1) % audio_device.rirb_entries);
    audio_device.rirb_read_pointer = read_pointer;
    //The RIRB should have 64 bit responses, but we only ever care about the bottom 32 bits...
    *response = audio_device.rirb[read_pointer*2];
    hda_log("RIRB response: 0x");
    hda_log_hex(NULL, *response, "\n");
    for( i = 1; i < 10; i++){
        if(audio_device.rirb[read_pointer*2 + i*2] != 0){
            hda_log("LOST RIRB SYNCHRONICITY!!!\n");
            hda_log("RIRB contents at ");
            hda_log_hex(NULL, i, ": 0x");
            hda_log_hex(NULL, audio_device.rirb[read_pointer*2 + i*2], "\n");

        }
    }

    hda_reg_writebyte(RIRBSTS, 5);
}

uint32_t HDA_codec_query(uint8_t codec, uint32_t nid, uint32_t payload){
    uint32_t response;
    uint32_t verb = ((codec & 0xF) << 28) |
                    ((nid & 0xFF) << 20) |
                    ((payload & 0xFFFFF));
    hda_log("\nCodec Query: 0x");
    hda_log_hex(NULL, verb, "\n");
    HDA_corb_write(verb);
    hda_log("Finished write\n");
    HDA_rirb_read(&response);
    hda_log("finished read: 0x");
    hda_log_hex(NULL, response, "\n");
    return response;
}
//
 void HDA_init_out_widget(){}
// void HDA_config_out_widget(){}
//
void HDA_widget_init(uint8_t codec, uint16_t node_id){
    uint32_t widget_capabilities;
    uint16_t widget_type;
    uint32_t amp_capabilities;
    uint32_t eapd_btl;
    widget_capabilities = HDA_codec_query(codec, node_id, VERB_GET_PARAMETER | PARAM_WIDGET_CAPABILITIES);
    //incapable widget. // new insult.
    if(widget_capabilities == 0){
        hda_log("FOUND INCAPABLE WIDGET\n");
        return;
    }
    //0xF00000 widget mask, 20 to get that widget in first few bits.
    widget_type = (uint16_t)((widget_capabilities & 0xF00000) >> 20);
    amp_capabilities = HDA_codec_query(codec, node_id, VERB_GET_PARAMETER | PARAM_OUT_AMP_CAP);
    eapd_btl = HDA_codec_query(codec, node_id, VERB_GET_EAPD_BTL);
    //format is now bit 7 = mute bit, 6:0 = gain
    hda_log("WIDGET_FOUND! 0x");
    hda_log_hex(NULL, widget_type, "\n");

    switch(widget_type){
        case WIDGET_OUTPUT          :{
            if(!audio_device.output.node_id){
                hda_log("OUTPUT FOUND\n");
                audio_device.output.codec = codec;
                audio_device.output.node_id = node_id;
                audio_device.output.amp_gain_steps = (int)((amp_capabilities >> 8) & 0x7F);

            }
            HDA_codec_query(codec, node_id, VERB_SET_EAPD_BTL | eapd_btl | 0x2);
        break;}
        case WIDGET_INPUT           :
        //Not supported

        break;
        case WIDGET_MIXER           :
        //Not supported

        break;
        case WIDGET_SELECTOR        :
        //Not supported

        break;
        case WIDGET_PIN_COMPLEX     :{
            uint32_t pin_capabilities;
            hda_log("PIN FOUND\n");

            pin_capabilities = HDA_codec_query(codec, node_id, VERB_GET_PARAMETER | PARAM_PIN_CAPABILITIES);

            //If it's the output pin, we care. Otherwise. toss it.
            if((pin_capabilities & (1 << 4)) == 0){
                return;
            }
            hda_log("OUTPUT PIN FOUND\n");

            uint32_t pin_cntl = HDA_codec_query(codec, node_id, VERB_GET_PIN_CONTROL);
            //6th bit enables the output amp stream
            pin_cntl |= (1 << 6);
            HDA_codec_query(codec, node_id, VERB_SET_PIN_CONTROL | pin_cntl);
            HDA_codec_query(codec, node_id, VERB_SET_EAPD_BTL | eapd_btl | 0x2);
        break;}
        case WIDGET_POWER           :
        //Not supported

        break;
        case WIDGET_VOLUME_KNOB     :
        //Not supported

        break;
        case WIDGET_BEEP_GEN        :
        //Not supported

        break;
        case WIDGET_VENDOR_DEFINED  :
        //Not supported

        break;
        default:
        //reserved
        break;
    }
    if(widget_capabilities & (0x1 << 10)){
        //If the widget has power control, turn it on!!! (normal mode)
        HDA_codec_query(codec, node_id, VERB_SET_POWER_STATE);
    }
}


int HDA_codec_list_widgets(uint8_t codec){
    uint32_t parameter;
    uint8_t func_grp_count;
    uint8_t init_func_grp_num;
    uint32_t i;
    uint32_t j;
    uint8_t widget_count;
    uint8_t init_widget_num;
    parameter = HDA_codec_query(codec, 0, VERB_GET_PARAMETER | PARAM_FUNCTION_GROUP_TYPE);
    hda_log("Function Group Type: 0x");
    hda_log_hex(NULL, parameter, "\n");
    parameter = HDA_codec_query(codec, 0, VERB_GET_PARAMETER | PARAM_SUB_NODE_COUNT);

    hda_log("Function group number: 0x");
    hda_log_hex(NULL, parameter, "\n");

    //lower 8 bits = number of subnodes for this codec
    func_grp_count = (uint8_t)(0xFF & parameter);
    //TODO: Why are all responses 0?
    init_func_grp_num = (uint8_t)(0xFF & (parameter >> 16));
    /*
    Do the above over again for all found sub nodes.
    peep the group type.?
    */
   hda_log("1\n");
    for(i = 0; i < func_grp_count; i++){
        parameter = HDA_codec_query(codec, (uint32_t)(init_func_grp_num + i), VERB_GET_PARAMETER | PARAM_SUB_NODE_COUNT);
        widget_count = (uint8_t)(parameter & 0xFF); // lower 8 bits
        init_widget_num = (uint8_t)((parameter >> 16) & 0xFF); // bits 23-16
        hda_log("Initial Widget Number: ");
        hda_log_hex(NULL, init_widget_num, "\n");
        hda_log("Widget count: ");
        hda_log_hex(NULL, widget_count, "\n");

        for(j = 0; j < init_widget_num; j++){
            HDA_widget_init(codec,  (uint16_t)(init_widget_num + j));
        }
    }
    hda_log("2\n");

    if(audio_device.output.node_id){
        return 0;
    }
    else{
        hda_log("3\n");

        return -1;
    }
}

void HDA_list_codecs(){

    uint16_t statests_reg = hda_reg_readword(STATESTS);
    uint8_t i;
    hda_log("\nSTATESTS_REG: 0x");
    hda_log_hex(NULL, statests_reg, "\n");
    //Don't have enoguh space to support many devices. just choose first.
    for(i = 0; i < SDIN_LEN; i++){
        if(statests_reg & (1 << i)){
            HDA_codec_list_widgets(i);
                return;
        }
    }
}

void HDA_reset(){
    //Clear out CTL bits for CORB and RIRB first
    //Reference code writes to 4 registers instead of just the control register.
    //weird. i guess it doesn't matter since they'll be overwritten anyways?
    hda_reg_writelong(CORBCTL, 0x0);
    hda_reg_writelong(RIRBCTL, 0x0);
    /* Wait for the DMA engines to be turned off*/
    while((hda_reg_readlong(CORBCTL) & CORBCTL_CORBRUN) | (hda_reg_readword(RIRBCTL) & RIRBCTL_RIRBRUN))
    {/* Wait . . . */};
    /* Done waiting! */

    //reset global control reg
    hda_reg_writelong(GCTL, 0x0);
    while(hda_reg_readlong(GCTL) & CRST)
    {/* Wait . . . */};
    hda_reg_writelong(GCTL, CRST);
    while((hda_reg_readlong(GCTL) & CRST) == 0)
    {/* Wait . . . */}
    //Enable all interrupts
    hda_reg_writeword(WAKEEN, 0xFFFF);
    hda_log("\nGCAP: 0x");
    hda_log_hex(NULL, hda_reg_readword(0), "\n");
    hda_log("WAKEEN: 0x");
    hda_log_hex(NULL, hda_reg_readword(WAKEEN), "\n");
    hda_log("\nreset_finished!\n");

    hda_log("Start of codec \n");
    hda_reg_writelong(INTCTL, 0x800003F);

    HDA_corb_init();
    HDA_rirb_init();
    HDA_list_codecs();

}
void HDA_set_default_volume(){
    HDA_set_volume(255); // max
}
void HDA_init_stream_descriptor(){
    // Placeholder for future stream descriptor initialization.
}

static bool hda_map_bar(pci_device_t* dev) {
    bool is_io = false;
    uint8_t bar_size_bits = 0;
    uint32_t bar0 = pci_read_bar(dev->bus, dev->slot, dev->func, 0, &is_io, &bar_size_bits);
    if (is_io || (bar0 & 0x1)) {
        hda_log("[hda] BAR0 is not MMIO; cannot initialize.\n");
        return false;
    }

    uint64_t base = (uint64_t)(bar0 & ~0xFu);
    bool is_64 = (bar0 & 0x6) == 0x4;
    if (is_64) {
        uint32_t bar1 = pci_config_read32(dev->bus, dev->slot, dev->func, 0x14);
        base |= ((uint64_t)bar1 << 32);
    }

    audio_device.mmio_size = bar_size_bits ? (1u << bar_size_bits) : 0;
    hda_mmio = (volatile uint8_t*)hhdm_phys_to_virt(base);
    audio_device.mmio_base = (uint8_t*)hda_mmio;

    hda_log("[hda] MMIO base: 0x");
    hda_log_hex(NULL, base, "\n");
    if (audio_device.mmio_size) {
        hda_log("[hda] MMIO size (1<<bits): 0x");
        hda_log_hex(NULL, audio_device.mmio_size, "\n");
    }

    return hda_mmio != NULL;
}

static bool hda_locate_controller(pci_device_t* dev_out) {
    if (pci_find_class_subclass(0x04, 0x03, dev_out)) {
        hda_log("[hda] Found HDA controller via class match.\n");
        return true;
    }
    hda_log("[hda] No HDA controller found.\n");
    return false;
}

void HDA_init_dev(){
    if (!hda_locate_controller(&g_hda_pci)) {
        return;
    }

    pci_enable_mmio_and_bus_mastering(g_hda_pci.bus, g_hda_pci.slot, g_hda_pci.func);
    if (!hda_map_bar(&g_hda_pci)) {
        return;
    }

    audio_device.rirb_read_pointer = 0;
    audio_device.buffer = kmalloc(1);
    HDA_reset();
    //TODO: the below.
    HDA_init_out_widget();
    HDA_init_stream_descriptor();

    HDA_set_default_volume();
}

void HDA_set_volume(uint8_t vol){
    (void)vol;
    return;
}

void hda_init(void) {
    HDA_init_dev();
}

// ===== End code based on https://github.com/inclementine/intelHDA =====
