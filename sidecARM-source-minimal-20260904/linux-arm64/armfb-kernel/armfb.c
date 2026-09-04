// SPDX-License-Identifier: GPL-2.0
/*
 * armfb.c - PiStorm/Emu68 shared-RAM framebuffer
 *
 * Linux renders an 800x600 RGB565 framebuffer directly into the
 * ARM-service shared RAM. Emu68/AmigaOS remains the owner of the
 * physical display hardware.
 */

#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>

#define ARMFB_WIDTH        800U
#define ARMFB_HEIGHT       600U
#define ARMFB_BPP          16U
#define ARMFB_BYTES_PP     2U
#define ARMFB_STRIDE       (ARMFB_WIDTH * ARMFB_BYTES_PP)
#define ARMFB_FRAME_BYTES  (ARMFB_STRIDE * ARMFB_HEIGHT)
#define ARMFB_PALETTE_SIZE 16U

struct armfb {
	struct fb_info *info;
	void __iomem *vram;
	resource_size_t phys;
	resource_size_t region_size;
	u32 pseudo_palette[ARMFB_PALETTE_SIZE];
};

static int armfb_check_var(struct fb_var_screeninfo *var,
			   struct fb_info *info)
{
	if (var->xres != ARMFB_WIDTH ||
	    var->yres != ARMFB_HEIGHT ||
	    var->bits_per_pixel != ARMFB_BPP)
		return -EINVAL;

	var->xres_virtual = ARMFB_WIDTH;
	var->yres_virtual = ARMFB_HEIGHT;
	var->xoffset = 0;
	var->yoffset = 0;

	var->red.offset = 11;
	var->red.length = 5;
	var->red.msb_right = 0;
	var->green.offset = 5;
	var->green.length = 6;
	var->green.msb_right = 0;
	var->blue.offset = 0;
	var->blue.length = 5;
	var->blue.msb_right = 0;
	var->transp.offset = 0;
	var->transp.length = 0;
	var->transp.msb_right = 0;

	var->nonstd = 0;
	var->activate = FB_ACTIVATE_NOW;
	var->height = -1;
	var->width = -1;
	var->accel_flags = 0;
	var->vmode = FB_VMODE_NONINTERLACED;

	return 0;
}

static int armfb_setcolreg(unsigned int regno,
			   unsigned int red,
			   unsigned int green,
			   unsigned int blue,
			   unsigned int transp,
			   struct fb_info *info)
{
	u32 value;

	if (regno >= ARMFB_PALETTE_SIZE)
		return -EINVAL;

	red >>= 16 - info->var.red.length;
	green >>= 16 - info->var.green.length;
	blue >>= 16 - info->var.blue.length;

	value = (red << info->var.red.offset) |
		(green << info->var.green.offset) |
		(blue << info->var.blue.offset);

	((u32 *)info->pseudo_palette)[regno] = value;
	return 0;
}

/*
 * Export the same physical ARMFB shared-memory window to userspace.
 *
 * Xorg's fbdev driver requires mmap() on /dev/fb0.  The kernel-side
 * framebuffer mapping is write-combining because VideoCore HVS scans
 * this RAM directly and does not snoop the ARM CPU cache.  Keep the
 * userspace mapping write-combining as well.
 */
static int armfb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct armfb *af = info->par;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset;
	unsigned long pfn;

	/*
	 * mmap() lengths are page-granular.  The visible framebuffer is
	 * 960000 bytes, which is not PAGE_SIZE-aligned, so Xorg/fbdev may
	 * legitimately request the final partial page as a full page.
	 *
	 * Validate against the actual shared-memory resource rather than
	 * the exact visible framebuffer byte count.
	 */
	if (vma->vm_pgoff > (~0UL >> PAGE_SHIFT))
		return -EINVAL;

	offset = vma->vm_pgoff << PAGE_SHIFT;

	if (offset >= af->region_size)
		return -EINVAL;

	if (size > af->region_size - offset)
		return -EINVAL;

	pfn = (af->phys >> PAGE_SHIFT) + vma->vm_pgoff;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	if (remap_pfn_range(vma, vma->vm_start, pfn, size,
			    vma->vm_page_prot))
		return -EAGAIN;

	return 0;
}

static const struct fb_ops armfb_ops = {
	.owner = THIS_MODULE,
	.fb_read = fb_sys_read,
	.fb_write = fb_sys_write,
	.fb_check_var = armfb_check_var,
	.fb_setcolreg = armfb_setcolreg,
	.fb_fillrect = sys_fillrect,
	.fb_copyarea = sys_copyarea,
	.fb_imageblit = sys_imageblit,
	.fb_mmap = armfb_mmap,
};

static int armfb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct fb_info *info;
	struct armfb *af;
	resource_size_t size;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "missing shared-memory resource\n");
		return -ENODEV;
	}

	size = resource_size(res);
	if (size < ARMFB_FRAME_BYTES) {
		dev_err(dev,
			"shared region too small: %pa bytes, need %u\n",
			&size, ARMFB_FRAME_BYTES);
		return -EINVAL;
	}

	info = framebuffer_alloc(sizeof(*af), dev);
	if (!info)
		return -ENOMEM;

	af = info->par;
	af->info = info;
	af->phys = res->start;
	af->region_size = size;

	/*
	 * HVS reads this RAM directly and does not snoop the ARM CPU cache.
	 * Map the framebuffer write-combining so fbcon writes reach RAM
	 * coherently without per-update cache maintenance.
	 */
	af->vram = devm_ioremap_wc(dev, res->start, ARMFB_FRAME_BYTES);
	if (IS_ERR(af->vram)) {
		ret = PTR_ERR(af->vram);
		dev_err(dev, "cannot map shared framebuffer: %d\n", ret);
		goto err_release;
	}

	memset_io(af->vram, 0, ARMFB_FRAME_BYTES);

	strscpy(info->fix.id, "armfb", sizeof(info->fix.id));
	info->fix.smem_start = res->start;
	info->fix.smem_len = ARMFB_FRAME_BYTES;
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.type_aux = 0;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.xpanstep = 0;
	info->fix.ypanstep = 0;
	info->fix.ywrapstep = 0;
	info->fix.line_length = ARMFB_STRIDE;
	info->fix.mmio_start = 0;
	info->fix.mmio_len = 0;
	info->fix.accel = FB_ACCEL_NONE;

	info->var.xres = ARMFB_WIDTH;
	info->var.yres = ARMFB_HEIGHT;
	info->var.xres_virtual = ARMFB_WIDTH;
	info->var.yres_virtual = ARMFB_HEIGHT;
	info->var.xoffset = 0;
	info->var.yoffset = 0;
	info->var.bits_per_pixel = ARMFB_BPP;
	info->var.grayscale = 0;
	info->var.red.offset = 11;
	info->var.red.length = 5;
	info->var.green.offset = 5;
	info->var.green.length = 6;
	info->var.blue.offset = 0;
	info->var.blue.length = 5;
	info->var.transp.offset = 0;
	info->var.transp.length = 0;
	info->var.nonstd = 0;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.height = -1;
	info->var.width = -1;
	info->var.accel_flags = 0;
	info->var.vmode = FB_VMODE_NONINTERLACED;

	info->fbops = &armfb_ops;
	info->flags = FBINFO_VIRTFB;
	info->screen_buffer = af->vram;
	info->screen_size = ARMFB_FRAME_BYTES;
	info->pseudo_palette = af->pseudo_palette;

	ret = register_framebuffer(info);
	if (ret) {
		dev_err(dev, "register_framebuffer failed: %d\n", ret);
		goto err_release;
	}

	platform_set_drvdata(pdev, info);

	dev_info(dev,
		 "fb%d registered: %ux%u RGB565, stride=%u, phys=%pa, region=%pa\n",
		 info->node, ARMFB_WIDTH, ARMFB_HEIGHT, ARMFB_STRIDE,
		 &af->phys, &af->region_size);

	return 0;

err_release:
	framebuffer_release(info);
	return ret;
}

static void armfb_remove(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);

	if (!info)
		return;

	unregister_framebuffer(info);
	framebuffer_release(info);
}

static const struct of_device_id armfb_of_match[] = {
	{ .compatible = "pistorm,armfb" },
	{ }
};
MODULE_DEVICE_TABLE(of, armfb_of_match);

static struct platform_driver armfb_driver = {
	.probe = armfb_probe,
	.remove = armfb_remove,
	.driver = {
		.name = "armfb",
		.of_match_table = armfb_of_match,
	},
};
module_platform_driver(armfb_driver);

MODULE_AUTHOR("sidecARM project");
MODULE_DESCRIPTION("PiStorm/Emu68 shared-RAM framebuffer");
MODULE_LICENSE("GPL");
