#include <windows.h>
#include <tchar.h>
#include <crass_types.h>
#include <acui.h>
#include <cui.h>
#include <package.h>
#include <resource.h>
#include <cui_error.h>
#include <stdio.h>

/* 接口数据结构: 表示cui插件的一般信息 */
struct acui_information J2WIN_cui_information = {
	_T(""),		/* copyright */
	_T(""),			/* system */
	_T(".DAT .LST .exe"),				/* package */
	_T("1.0.0"),			/* revision */
	_T(""),			/* author */
	_T(""),	/* date */
	NULL,					/* notion */
	ACUI_ATTRIBUTE_LEVEL_STABLE
};

/* 所有的封包特定的数据结构都要放在这个#pragma段里 */
#pragma pack (1)
typedef struct {
	s8 name[14];
	u32 offset;
	u32 length;
} LST_entry_t;

typedef struct {
	s8 magic[16];	// "FFA SYSTEM G-SYS"
	u32 zero;
	u32 reserved;
	u32 data_offset;
	u32 data_length;
	u32 lst_offset;
	u32 lst_size;
	u32 tag_offset;
	u32 tag_size;
	s8 magic2[16];	// "ffa system g-sys"
} exe_tailer_t;

typedef struct {
	u32 version;	// 0, 1 or 2
	
} pt1_header_t;
#pragma pack ()


static void lzss_uncompress(BYTE *uncompr, DWORD uncomprlen, 
							 BYTE *compr, DWORD comprlen)
{
	DWORD act_uncomprlen = 0;
	/* compr中的当前字节中的下一个扫描位的位置 */
	DWORD curbit = 0;
	/* compr中的当前扫描字节 */
	DWORD curbyte = 0;
	DWORD nCurWindowByte = 0xfee;
	BYTE win[4096];
	WORD flag = 0;

	BYTE *p = win;
	for (DWORD i = 0; i < 256; i++) {
		for (DWORD k = 0; k < 13; k++)
			*p++ = (BYTE)i;
	}
	for (i = 0; i < 256; i++)
		*p++ = (BYTE)i;
	for (i = 0; i < 256; i++)
		*p++ = 255 - (BYTE)i;
	memset(p, 0, 128);
	p += 128;
	memset(p, ' ', 110);
	p += 110;
	memset(p, 0, 18);

	while (1) {
		flag >>= 1;
		if (!(flag & 0x0100))
			flag = compr[curbyte++] | 0xff00;

		if (act_uncomprlen >= uncomprlen)
			break;

		if (!(flag & 1)) {
			DWORD win_offset = compr[curbyte++];
			DWORD copy_bytes = compr[curbyte++];

			win_offset |= (copy_bytes & 0xf0) << 4;
			copy_bytes &= 0x0f;
			copy_bytes += 3;
			if (act_uncomprlen + copy_bytes > uncomprlen) {
				copy_bytes = uncomprlen - act_uncomprlen;
				if (!copy_bytes)
					break;
			}

			for (i = 0; i < copy_bytes; i++) {	
				BYTE data = win[(win_offset + i) & 0xfff];
				uncompr[act_uncomprlen++] = data;
				/* 输出的1字节放入滑动窗口 */
				win[nCurWindowByte++] = data;
				nCurWindowByte &= 0xfff;	
			}
		} else {
			BYTE data = compr[curbyte++];
			uncompr[act_uncomprlen++] = data;
			/* 输出的1字节放入滑动窗口 */
			win[nCurWindowByte++] = data;
			nCurWindowByte &= 0xfff;			
		}
	}
}

/********************* DAT *********************/

/* 封包匹配回调函数 */
static int J2WIN_DAT_match(struct package *pkg)
{
	if (pkg->pio->open(pkg, IO_READONLY))
		return -CUI_EOPEN;

	if (pkg->pio->open(pkg->lst, IO_READONLY)) {
		pkg->pio->close(pkg);
		return -CUI_EOPEN;
	}

	return 0;	
}

/* 封包索引目录提取函数 */
static int J2WIN_DAT_extract_directory(struct package *pkg,
									   struct package_directory *pkg_dir)
{
	LST_entry_t *index_buffer;
	SIZE_T LST_size;

	if (pkg->pio->length_of(pkg->lst, &LST_size))
		return -CUI_ELEN;

	index_buffer = (LST_entry_t *)malloc(LST_size);
	if (!index_buffer)
		return -CUI_EMEM;

	if (pkg->pio->read(pkg->lst, index_buffer, LST_size)) {
		free(index_buffer);
		return -CUI_EREAD;
	}

	pkg_dir->index_entries = LST_size / sizeof(LST_entry_t);
	pkg_dir->directory = index_buffer;
	pkg_dir->directory_length = LST_size;
	pkg_dir->index_entry_length = sizeof(LST_entry_t);

	return 0;
}

/* 封包索引项解析函数 */
static int J2WIN_DAT_parse_resource_info(struct package *pkg,
										 struct package_resource *pkg_res)
{
	LST_entry_t *LST_entry;

	LST_entry = (LST_entry_t *)pkg_res->actual_index_entry;
	if (!LST_entry->name[13])
		pkg_res->name_length = strlen(LST_entry->name);
	else
		pkg_res->name_length = 14;
	strncpy(pkg_res->name, LST_entry->name, pkg_res->name_length);
	pkg_res->raw_data_length = LST_entry->length;
	pkg_res->actual_data_length = 0;
	pkg_res->offset = LST_entry->offset;

	return 0;
}

/* 封包资源提取函数 */
static int J2WIN_DAT_extract_resource(struct package *pkg,
									  struct package_resource *pkg_res)
{
	pkg_res->raw_data = (BYTE *)malloc(pkg_res->raw_data_length);
	if (!pkg_res->raw_data)
		return -CUI_EMEM;

	if (pkg->pio->readvec(pkg, pkg_res->raw_data, pkg_res->raw_data_length, pkg_res->offset, IO_SEEK_SET)) {
		free(pkg_res->raw_data);
		pkg_res->raw_data = NULL;
		return -CUI_EREADVEC;
	}

	return 0;
}

/* 资源保存函数 */
static int J2WIN_DAT_save_resource(struct resource *res, 
								   struct package_resource *pkg_res)
{
	if (!res || !pkg_res)
		return -CUI_EPARA;
	
	if (res->rio->create(res))
		return -CUI_ECREATE;

	if (pkg_res->actual_data && pkg_res->actual_data_length) {
		if (res->rio->write(res, pkg_res->actual_data, pkg_res->actual_data_length)) {
			res->rio->close(res);
			return -CUI_EWRITE;
		}
	} else if (pkg_res->raw_data && pkg_res->raw_data_length) {
		if (res->rio->write(res, pkg_res->raw_data, pkg_res->raw_data_length)) {
			res->rio->close(res);
			return -CUI_EWRITE;
		}
	}

	res->rio->close(res);
	
	return 0;
}

/* 封包资源释放函数 */
static void J2WIN_DAT_release_resource(struct package *pkg, 
									   struct package_resource *pkg_res)
{
	if (pkg_res->raw_data) {
		free(pkg_res->raw_data);
		pkg_res->raw_data = NULL;
	}
}

/* 封包卸载函数 */
static void J2WIN_DAT_release(struct package *pkg, 
							  struct package_directory *pkg_dir)
{
	if (pkg_dir->directory) {
		free(pkg_dir->directory);
		pkg_dir->directory = NULL;
	}

	pkg->pio->close(pkg->lst);
	pkg->pio->close(pkg);
}

/* 封包处理回调函数集合 */
static cui_ext_operation J2WIN_DAT_operation = {
	J2WIN_DAT_match,				/* match */
	J2WIN_DAT_extract_directory,	/* extract_directory */
	J2WIN_DAT_parse_resource_info,	/* parse_resource_info */
	J2WIN_DAT_extract_resource,		/* extract_resource */
	J2WIN_DAT_save_resource,		/* save_resource */
	J2WIN_DAT_release_resource,		/* release_resource */
	J2WIN_DAT_release				/* release */
};

/********************* exe *********************/

/* 封包匹配回调函数 */
static int J2WIN_exe_match(struct package *pkg)
{
	exe_tailer_t tailer;

	if (pkg->pio->open(pkg, IO_READONLY))
		return -CUI_EOPEN;

	if (pkg->pio->seek(pkg, 0 - sizeof(exe_tailer_t), IO_SEEK_END)) {
		pkg->pio->close(pkg);
		return -CUI_ESEEK;
	}

	if (pkg->pio->read(pkg, &tailer, sizeof(exe_tailer_t))) {
		pkg->pio->close(pkg);
		return -CUI_EREAD;
	}

	if (strncmp(tailer.magic, "FFA SYSTEM G-SYS", sizeof(tailer.magic))  
			|| strncmp(tailer.magic2, "ffa system g-sys", sizeof(tailer.magic2))
			|| tailer.zero) {
		pkg->pio->close(pkg);
		return -CUI_EMATCH;
	}
	
	return 0;	
}

/* 封包索引目录提取函数 */
static int J2WIN_exe_extract_directory(struct package *pkg,
									   struct package_directory *pkg_dir)
{
	exe_tailer_t tailer;
	LST_entry_t *index_buffer;

	if (pkg->pio->seek(pkg, 0 - sizeof(exe_tailer_t), IO_SEEK_END))
		return -CUI_ESEEK;

	if (pkg->pio->read(pkg, &tailer, sizeof(exe_tailer_t)))
		return -CUI_EREAD;

	if (pkg->pio->seek(pkg, tailer.lst_offset, IO_SEEK_SET))
		return -CUI_ESEEK;

	index_buffer = (LST_entry_t *)malloc(tailer.lst_size);
	if (!index_buffer)
		return -CUI_EMEM;

	if (pkg->pio->read(pkg, index_buffer, tailer.lst_size)) {
		free(index_buffer);
		return -CUI_EREAD;
	}

	DWORD index_entries = tailer.lst_size / sizeof(LST_entry_t);
	for (DWORD i = 0; i < index_entries; i++) {
		index_buffer[i].offset += tailer.data_offset;
	}

	pkg_dir->index_entries = tailer.lst_size / sizeof(LST_entry_t);
	pkg_dir->directory = index_buffer;
	pkg_dir->directory_length = tailer.lst_size;
	pkg_dir->index_entry_length = sizeof(LST_entry_t);
	//pkg_dir->flags = PKG_DIR_FLAG_SKIP0;

	return 0;
}

/* 封包卸载函数 */
static void J2WIN_exe_release(struct package *pkg, 
							  struct package_directory *pkg_dir)
{
	if (pkg_dir->directory) {
		free(pkg_dir->directory);
		pkg_dir->directory = NULL;
	}

	pkg->pio->close(pkg);
}

/* 封包处理回调函数集合 */
static cui_ext_operation J2WIN_exe_operation = {
	J2WIN_exe_match,				/* match */
	J2WIN_exe_extract_directory,	/* extract_directory */
	J2WIN_DAT_parse_resource_info,	/* parse_resource_info */
	J2WIN_DAT_extract_resource,		/* extract_resource */
	J2WIN_DAT_save_resource,		/* save_resource */
	J2WIN_DAT_release_resource,		/* release_resource */
	J2WIN_exe_release				/* release */
};

/********************* SO4 *********************/

/* 封包匹配回调函数 */
static int J2WIN_SO4_match(struct package *pkg)
{
	if (pkg->pio->open(pkg, IO_READONLY))
		return -CUI_EOPEN;

	return 0;	
}

/* 封包资源提取函数 */
static int J2WIN_SO4_extract_resource(struct package *pkg,
									  struct package_resource *pkg_res)
{
	BYTE *compr, *uncompr;
	DWORD comprlen, uncomprlen;

	if (pkg->pio->readvec(pkg, &comprlen, 4, 0, IO_SEEK_SET)) 
		return -CUI_EREADVEC;

	if (pkg->pio->readvec(pkg, &uncomprlen, 4, 4, IO_SEEK_SET)) 
		return -CUI_EREADVEC;

	compr = (BYTE *)malloc(comprlen);
	if (!compr)
		return -CUI_EMEM;

	if (pkg->pio->readvec(pkg, compr, comprlen, 8, IO_SEEK_SET)) {
		free(compr);
		return -CUI_EREADVEC;
	}

	uncompr = (BYTE *)malloc(uncomprlen);
	if (!uncompr) {
		free(compr);
		return -CUI_EMEM;
	}

	lzss_uncompress(uncompr, uncomprlen, compr, comprlen);

	pkg_res->raw_data = compr;
	pkg_res->raw_data_length = comprlen;
	pkg_res->actual_data = uncompr;
	pkg_res->actual_data_length = uncomprlen;

	return 0;
}

/* 封包资源释放函数 */
static void J2WIN_SO4_release_resource(struct package *pkg, 
									   struct package_resource *pkg_res)
{
	if (pkg_res->actual_data) {
		free(pkg_res->actual_data);
		pkg_res->actual_data = NULL;
	}
	if (pkg_res->raw_data) {
		free(pkg_res->raw_data);
		pkg_res->raw_data = NULL;
	}
}

/* 封包卸载函数 */
static void J2WIN_SO4_release(struct package *pkg, 
							  struct package_directory *pkg_dir)
{
	pkg->pio->close(pkg);
}

/* 封包处理回调函数集合 */
static cui_ext_operation J2WIN_SO4_operation = {
	J2WIN_SO4_match,				/* match */
	NULL,							/* extract_directory */
	NULL,							/* parse_resource_info */
	J2WIN_SO4_extract_resource,		/* extract_resource */
	J2WIN_DAT_save_resource,		/* save_resource */
	J2WIN_SO4_release_resource,		/* release_resource */
	J2WIN_SO4_release				/* release */
};

/********************* WA1 *********************/

/* 封包资源提取函数 */
static int J2WIN_WA1_extract_resource(struct package *pkg,
									  struct package_resource *pkg_res)
{
	BYTE *compr, *uncompr;
	DWORD comprlen, uncomprlen;
	SIZE_T wa1_size;

	if (pkg->pio->length_of(pkg, &wa1_size)) 
		return -CUI_ELEN;

	if (pkg->pio->readvec(pkg, &comprlen, 4, 0, IO_SEEK_SET)) 
		return -CUI_EREADVEC;

	if (comprlen == wa1_size - 8) {
		if (pkg->pio->readvec(pkg, &uncomprlen, 4, 4, IO_SEEK_SET)) 
			return -CUI_EREADVEC;

		compr = (BYTE *)malloc(comprlen);
		if (!compr)
			return -CUI_EMEM;

		if (pkg->pio->readvec(pkg, compr, comprlen, 8, IO_SEEK_SET)) {
			free(compr);
			return -CUI_EREADVEC;
		}

		uncompr = (BYTE *)malloc(uncomprlen);
		if (!uncompr) {
			free(compr);
			return -CUI_EMEM;
		}

		lzss_uncompress(uncompr, uncomprlen, compr, comprlen);
	} else {
		uncomprlen = wa1_size - 4;
		uncompr = (BYTE *)malloc(uncomprlen);
		if (!uncompr)
			return -CUI_EMEM;

		if (pkg->pio->readvec(pkg, uncompr, uncomprlen, 4, IO_SEEK_SET)) {
			free(uncompr);
			return -CUI_EREADVEC;
		}
		compr = NULL;
		comprlen = wa1_size;
	}
	pkg_res->flags |= PKG_RES_FLAG_APEXT;
	pkg_res->replace_extension = _T(".wav");
	pkg_res->raw_data = compr;
	pkg_res->raw_data_length = comprlen;
	pkg_res->actual_data = uncompr;
	pkg_res->actual_data_length = uncomprlen;

	return 0;
}

/* 封包处理回调函数集合 */
static cui_ext_operation J2WIN_WA1_operation = {
	J2WIN_SO4_match,				/* match */
	NULL,							/* extract_directory */
	NULL,							/* parse_resource_info */
	J2WIN_WA1_extract_resource,		/* extract_resource */
	J2WIN_DAT_save_resource,		/* save_resource */
	J2WIN_SO4_release_resource,		/* release_resource */
	J2WIN_SO4_release				/* release */
};


/* 接口函数: 向cui_core注册支持的封包类型 */
int CALLBACK J2WIN_register_cui(struct cui_register_callback *callback)
{
	/* 注册cui插件支持的扩展名、资源放入扩展名、处理回调函数和封包属性 */
	if (callback->add_extension(callback->cui, _T(".DAT"), NULL, 
		NULL, &J2WIN_DAT_operation, CUI_EXT_FLAG_PKG | CUI_EXT_FLAG_DIR | CUI_EXT_FLAG_LST))
			return -1;

	if (callback->add_extension(callback->cui, _T(".exe"), NULL, 
		NULL, &J2WIN_exe_operation, CUI_EXT_FLAG_PKG | CUI_EXT_FLAG_DIR))
			return -1;

	// type1 - 可压缩可不压缩（不压缩时没有8字节压缩头）
	if (callback->add_extension(callback->cui, _T(".WA1"), NULL, 
		NULL, &J2WIN_WA1_operation, CUI_EXT_FLAG_PKG | CUI_EXT_FLAG_RES))
			return -1;

	// typeX（.SO4是0） - 压缩
	if (callback->add_extension(callback->cui, _T(".SO4"), NULL, 
		NULL, &J2WIN_SO4_operation, CUI_EXT_FLAG_PKG | CUI_EXT_FLAG_RES))
			return -1;

	// type2 - 不压缩
//	if (callback->add_extension(callback->cui, _T(".PT1"), NULL, 
//		NULL, &J2WIN_pt1_operation, CUI_EXT_FLAG_PKG | CUI_EXT_FLAG_RES))
//			return -1;

	return 0;
}
