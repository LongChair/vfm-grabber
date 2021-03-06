#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/major.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/of_reserved_mem.h>

#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/delay.h>

#include <linux/amlogic/canvas/canvas.h>

#include "vfm_grabber.h"


// Module version
#define VERSION_MAJOR 0
#define VERSION_MINOR 0

// Names / devices and general config

#define DRIVER_NAME     "vfm_grabber"
#define MODULE_NAME     DRIVER_NAME
#define DEVICE_NAME     DRIVER_NAME
#define RECEIVER_NAME   DRIVER_NAME
#define DEBUG_LOGFILE 	"/storage/vfm_grabber.log"

#define MEM_INPUT_MAX_SIZE      (4 * 1024 * 1024)	// Maximum JPEG input data size
#define MEM_OUTPUT_MAX_SIZE     (4 * 3840 * 2160)	// Maximum decoded output size (4k)

// types definition
typedef struct
{
  unsigned long start;
  unsigned long size;
  unsigned long input_start;
  unsigned long output_start;
} reserved_mem_s;

// Variables
static vfm_grabber_dev grabber_dev;
//static struct class *vfm_grabber_class;
//static struct device *vfm_grabber_dev;
//static struct vframe_receiver_s vfm_grabber_vf_receiver;

static reserved_mem_s reserved_mem;

//////////////////////////////////////////////////
// Log functions 
//////////////////////////////////////////////////
static struct file* logfile = NULL;	// Current logfile pointer
static loff_t log_pos = 0;		// Current logfile position


// this function writes in a user file
void debug_log(char *prefix, char *format, ...)
{
  char logstr[300];
  char fullstr[512];
  va_list args;
  mm_segment_t old_fs;

  if (!logfile)
  {
    logfile = filp_open(DEBUG_LOGFILE, O_RDWR | O_CREAT, 0644);
    if (!logfile) return;
  }

  va_start(args, format);
  vsprintf(logstr, format, args);
  sprintf(fullstr,"%s : %s", prefix, logstr);
  va_end (args);

  old_fs = get_fs();
  set_fs(KERNEL_DS);
  vfs_write(logfile, fullstr, strlen(fullstr), &log_pos);
  set_fs(old_fs);
}

// this function writes in the kernel log
void system_log(int logtype, char *prefix, char *format, ...)
{
  char logstr[300];
  char fullstr[512];
  va_list args;

  va_start(args, format);
  vsprintf(logstr, format, args);
  sprintf(fullstr,"%s : %s", prefix, logstr);
  va_end (args);

  if (logtype==0)
    pr_info("%s", fullstr);
  else
    pr_err("%s", fullstr);
}

//#define DEBUG
#ifdef DEBUG
#define log_info(fmt, arg...)  	debug_log("info", fmt, ## arg)
#define log_error(fmt, arg...) 	debug_log("error", fmt, ## arg)
#else
#define log_info(fmt, arg...) 	system_log(0, DRIVER_NAME, fmt, ## arg) 
#define log_error(fmt, arg...)  system_log(1, DRIVER_NAME, fmt, ## arg)
#endif

// functions prototypes
int get_vf_size(struct vframe_s *vf);

//////////////////////////////////////////////////
// DMABUF operations functions
//////////////////////////////////////////////////

static int vfm_grabber_attach_dma_buf(struct dma_buf *dmabuf,
  struct device *dev,
  struct dma_buf_attachment *attach)
{
  log_info("vfm_grabber_attach_dma_buf priv=%p\n", dmabuf->priv);
  attach->priv = dmabuf->priv;
  return 0;
}

static void vfm_grabber_detach_dma_buf(struct dma_buf *dmabuf,
  struct dma_buf_attachment *attach)
{
  log_info("vfm_grabber_detach_dma_buf priv %p\n", attach->priv);
}

static struct sg_table *
vfm_grabber_map_dma_buf(struct dma_buf_attachment *attach,
  enum dma_data_direction dir)
{
  int ret;
  struct sg_table *sgt = NULL;
  struct vframe_s *vf = (struct vframe_s*)attach->priv;

  log_info("vfm_grabber_map_dma_buf\n");

  if (!vf)
  {
    log_error("vfm_grabber_map_dma_buf : vf is NULL");
    return NULL;
  }

  // TODO: figure out how to clean this pointer up
  sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
  if (!sgt)
  {
    log_error("vfm_grabber_map_dma_buf: kzalloc failed.\n");
    return NULL;
  }

//  // CMA memory will always have a single entry
  ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
  if (ret)
  {
    log_error("failed to alloc sgt.\n");
    return NULL;
  }

//  // CMA memory should always be page aligned and,
//  // therefore, always have a 0 offset
  struct canvas_s cs0;
  canvas_read(vf->canvas0Addr & 0xff, &cs0);

  sg_set_page(sgt->sgl,phys_to_page(cs0.addr), vf->width * vf->height, 0);

  sg_dma_address(sgt->sgl) = cs0.addr;
  sg_dma_len(sgt->sgl) = vf->width * vf->height;

  pr_info("vfm_grabber_map_dma_buf: vf : %d x %d\n",vf->width, vf-> height);
  pr_info("vfm_grabber_map_dma_buf: cs0 : %d x %d, cs1 : %d x %d\n", cs0.width, cs0.height, cs1.width, cs1.height);
  pr_info("vfm_grabber_map_dma_buf: sgt=%p, page=%p (%p), size=%d\n",
    sgt,
    (void*)phys_to_page(vf->canvas0Addr),
    (void*)cs0.addr,
    get_vf_size(vf));

  return sgt;
}

static void vfm_grabber_unmap_dma_buf(struct dma_buf_attachment *attach,
  struct sg_table *sgt,
  enum dma_data_direction dir)
{
  // TODO: Do we clean up the sg_table* ?
  log_info("vfm_grabber_unmap_dma_buf\n");
  if (sgt)
   kfree(sgt);
}


static void *vfm_grabber_dmabuf_kmap_atomic(struct dma_buf *dma_buf,
  unsigned long page_num)
{
  /* TODO */
  log_info("vfm_grabber_dmabuf_kmap_atomic\n");
  return NULL;
}

static void vfm_grabber_dmabuf_kunmap_atomic(struct dma_buf *dma_buf,
  unsigned long page_num,
  void *addr)
{
  /* TODO */
  log_info("vfm_grabber_dmabuf_kunmap_atomic\n");
}

static void *vfm_grabber_dmabuf_kmap(struct dma_buf *dma_buf,
  unsigned long page_num)
{
  /* TODO */
  log_info("vfm_grabber_dmabuf_kmap\n");
  return NULL;
}

static void vfm_grabber_dmabuf_kunmap(struct dma_buf *dma_buf,
  unsigned long page_num, void *addr)
{
  /* TODO */
  log_info("vfm_grabber_dmabuf_kunmap\n");
}

static int vfm_grabber_dmabuf_mmap(struct dma_buf *dma_buf,
  struct vm_area_struct *vma)
{
  log_info("vfm_grabber_mmap\n");
  return 0;
}

static void vfm_grabber_dmabuf_release(struct dma_buf *dma_buf)
{
  // TODO
  log_info("vfm_grabber_dmabuf_release\n");
}

static struct dma_buf_ops vfm_grabber_dmabuf_ops = {
  .attach = vfm_grabber_attach_dma_buf,
  .detach = vfm_grabber_detach_dma_buf,
  .map_dma_buf = vfm_grabber_map_dma_buf,
  .unmap_dma_buf = vfm_grabber_unmap_dma_buf,
  .kmap = vfm_grabber_dmabuf_kmap,
  .kmap_atomic = vfm_grabber_dmabuf_kmap_atomic,
  .kunmap = vfm_grabber_dmabuf_kunmap,
  .kunmap_atomic = vfm_grabber_dmabuf_kunmap_atomic,
  .mmap = vfm_grabber_dmabuf_mmap,
 .release = vfm_grabber_dmabuf_release,
 };

int create_dmabuf(vfm_grabber_dev *dev, int index,struct vframe_s *vf)
{
  struct dma_buf* dmabuf;
  int flags = 0;

  log_info("Creating dmabuf #%d on %p (size : %d)\n", index, vf->canvas0Addr, get_vf_size(vf));

  dmabuf = dma_buf_export(vf, &vfm_grabber_dmabuf_ops, get_vf_size(vf),	flags);
  if (!dmabuf)
  {
    log_error("Failed to create dmabuf\n");
    return -1;
  }

  dev->buffer[index].dmabuf = dmabuf;
  dev->buffer[index].dma_fd = dma_buf_fd(dmabuf, flags);

  if (dev->buffer[index].dma_fd <= 0)
  {
    log_error("Failed to create dmabuf file descriptor\n");
    return -1;
  }

  return 0;
}

void release_dmabuf(vfm_grabber_dev *dev, int index)
{
  if (dev->buffer[index].dma_fd)
  {
    log_info("Releasing DMABUF #%d\n", index);
    dma_buf_put(dev->buffer[index].dmabuf);
    dev->buffer[index].dmabuf = NULL;
    dev->buffer[index].dma_fd = 0;
  }
}


//////////////////////////////////////////////////
// VFM operations functions
//////////////////////////////////////////////////

int get_vf_size(struct vframe_s *vf)
{
  if (vf)
  {
    if (vf->type & VIDTYPE_VIU_NV21)
    {
        return (vf->width * vf->height) + ((vf->width * vf->height) >> 1);
    }
  }

  return 0;
}

static int vfm_grabber_receiver_event_fun(int type, void *data, void *private_data)
{

  vfm_grabber_dev *dev = (vfm_grabber_dev *)private_data;

  static struct timeval frametime;
  int elapsedtime, i;

  log_info("Got VFM event %d \n", type);

  switch(type)
  {
    case VFRAME_EVENT_PROVIDER_UNREG:
      for (i=0; i < MAX_DMABUF_FD; i++)
        release_dmabuf(dev, i);
    break;

    case VFRAME_EVENT_PROVIDER_REG:
    break;

    case VFRAME_EVENT_PROVIDER_START:
      dev->framecount = 0;
      dev->info.frames_decoded = 0;
      dev->info.frames_ready = 0;
    break;

    case VFRAME_EVENT_PROVIDER_QUREY_STATE:
     return RECEIVER_ACTIVE;
    break;

    case VFRAME_EVENT_PROVIDER_VFRAME_READY:
      dev->info.frames_ready++;
      dev->info.frames_decoded++;

      if (dev->framecount == 0)
        do_gettimeofday(&dev->starttime);

      do_gettimeofday(&frametime);
      elapsedtime = (frametime.tv_sec * 1000000 + frametime.tv_usec) - (dev->starttime.tv_sec * 1000000 + dev->starttime.tv_usec);

      dev->framecount++;

      log_info("Got VFRAME_EVENT_PROVIDER_VFRAME_READY, Framerate = %d / %d\n", dev->framecount, elapsedtime);
    break;

  }

  return 0;
}

static const struct vframe_receiver_op_s vfm_grabber_vf_receiver = {.event_cb =
  vfm_grabber_receiver_event_fun};

//////////////////////////////////////////////////
// File operations functions
//////////////////////////////////////////////////
static int vfm_grabber_mmap(struct file *filp, struct vm_area_struct *vma)
{
  int ret = 0;
  return ret;
}

static long vfm_grabber_ioctl(struct file *file, unsigned int cmd, ulong arg)
{
  int ret = 0;
  vfm_grabber_frame frame = { 0 };
  struct vframe_s *vf;
  vfm_grabber_dev *dev = (vfm_grabber_dev *)(&grabber_dev);
  int count = 20;

  switch(cmd)
  {
    case VFM_GRABBER_GET_FRAME:

      log_info("VFM_GRABBER_GET_FRAME ioctl called\n");
      while (count)
      {
        vf = vf_get(RECEIVER_NAME);
        if (vf)
        {
          log_info("VFM_GRABBER_GET_FRAME ioctl peeked frame of type %d\n", vf->type);

          // create the dmabuf fd if it's not been created yet
          if (dev->buffer[vf->index].dma_fd == 0)
          {
            if (create_dmabuf(dev, vf->index, vf) < 0)
              return -1;
          }

          frame.dma_fd = dev->buffer[vf->index].dma_fd;
          frame.width = vf->width;
          frame.height = vf->height;
          frame.stride = vf->width;
          frame.priv = vf;

          dev->info.frames_ready--;
          vf_put(vf, RECEIVER_NAME);

          return copy_to_user((void*)arg, &frame, sizeof(frame));
          break;
        }
        else
        {
          msleep(5);
          count--;
        }
      }

      return count == 0 ? -1 : 0;
      break;

   case VFM_GRABBER_GET_INFO:
      return copy_to_user((void*)arg, &dev->info, sizeof(dev->info));
    break;
  }

  return ret;
}

static int vfm_grabber_open(struct inode *inode, struct file *file)
{
  int ret = 0;
  return ret;
}

static int vfm_grabber_release(struct inode *inode, struct file *file)
{
  int ret = 0;
  return ret;
}


static const struct file_operations vfm_grabber_fops =
{
  .owner = THIS_MODULE,
  .open = vfm_grabber_open,
  .mmap = vfm_grabber_mmap,
  .release = vfm_grabber_release,
  .unlocked_ioctl = vfm_grabber_ioctl,
};

//////////////////////////////////////////////////
// Probe and remove functions 
//////////////////////////////////////////////////
static int vfm_grabber_probe(struct platform_device *pdev)
{
  int ret;
//  // gets the work memory area
//  ret = of_reserved_mem_device_init(&pdev->dev);
//  if (ret == 0)
//  {
//     log_error("failed to retrieve reserved memory.\n");
//     return -EFAULT;
//  }
//  log_info("reserved memory retrieved successfully.\n");
  memset(&grabber_dev, 0, sizeof(grabber_dev));

  ret = register_chrdev(VERSION_MAJOR, DEVICE_NAME, &vfm_grabber_fops);
  if (ret < 0) 
  {
     log_error("can't register major for device\n");
     return ret;
  }

  grabber_dev.version_major =  ret;

  grabber_dev.device_class = class_create(THIS_MODULE, DEVICE_NAME);
  if (!grabber_dev.device_class)
  {
    log_error("failed to create class\n");
    return -EFAULT;
  }

  grabber_dev.file_device = device_create(grabber_dev.device_class, NULL,
                                        MKDEV(grabber_dev.version_major, VERSION_MINOR),
                                        NULL, DEVICE_NAME);
  if (!grabber_dev.file_device)
  {
    log_error("failed to create device %s", DEVICE_NAME);
    return -EFAULT;
  }

  // register to vfm
  vf_receiver_init(&grabber_dev.vfm_vf_receiver, RECEIVER_NAME, &vfm_grabber_vf_receiver, &grabber_dev);
  vf_reg_receiver(&grabber_dev.vfm_vf_receiver);

  log_info("driver probed successfully\n");
  return 0;
}

static int vfm_grabber_remove(struct platform_device *pdev)
{
  // unregister from vfm
  vf_unreg_receiver(&grabber_dev.vfm_vf_receiver);
  //vf_receiver_free(&grabber_dev.vfm_vf_receiver);

  device_destroy(grabber_dev.device_class, MKDEV(grabber_dev.version_major, VERSION_MINOR));

  class_destroy(grabber_dev.device_class);

  unregister_chrdev(VERSION_MAJOR, DEVICE_NAME);

  return 0;
}

//////////////////////////////////////////////////
// Module Init / Exit functions 
//////////////////////////////////////////////////
static const struct of_device_id vfm_grabber_dt_match[] =
{
  {
    .compatible = "amlogic, vfm_grabber",
  },
  {},
};

static struct platform_driver vfm_grabber_driver =
{
  .probe = vfm_grabber_probe,
  .remove = vfm_grabber_remove,
  .driver = 
  {
    .name = DRIVER_NAME,
    .owner = THIS_MODULE,
    .of_match_table = vfm_grabber_dt_match,
  }
};


static int __init vfm_grabber_init(void)
{
  if (platform_driver_register(&vfm_grabber_driver))
  {
    log_error("failed to register vfm_grabber module\n");
    return -ENODEV;
  }

  log_info("module initialized successfully\n");
  return 0;
}

static void __exit vfm_grabber_exit(void)
{
  platform_driver_unregister(&vfm_grabber_driver);
  log_info("module exited\n");
  return;
}

module_init(vfm_grabber_init);
module_exit(vfm_grabber_exit);

//////////////////////////////////////////////////
// Memory Setup
//////////////////////////////////////////////////
static int vfm_grabber_mem_device_init(struct reserved_mem *rmem, struct device *dev)
{
   memset(&reserved_mem, 0, sizeof(reserved_mem_s));
   reserved_mem.start = rmem->base;
   reserved_mem.size = rmem->size;
   log_info("memory resource found at %lx (%lx)\n", reserved_mem.start, reserved_mem.size);
   return 0;
}

static const struct reserved_mem_ops rmem_vfm_grabber_ops =
{
  .device_init = vfm_grabber_mem_device_init,
};

static int __init vfm_grabber_mem_setup(struct reserved_mem *rmem)
{
  rmem->ops = &rmem_vfm_grabber_ops;
  log_info("doing share mem setup\n");
  return 0;
}

//////////////////////////////////////////////////
// Module declaration
//////////////////////////////////////////////////
RESERVEDMEM_OF_DECLARE(DRIVER_NAME, "amlogic, vfm_grabber_memory", vfm_grabber_mem_setup);
MODULE_DESCRIPTION("VFM Grabber driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lionel CHAZALLON <LongChair@hotmail.com>");

