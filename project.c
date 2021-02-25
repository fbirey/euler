#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#define FILENAME_MAX 8
#define INODE_MAX 8192
#define MAX_DATA_INBLOCK 360
#define BLOCK_MAX 20480
#define DATA_MAP_BASE 1536

const char* disk_path = "/tmp/diskimg"
static int inode_base = 4096;
static int nothing = 0;
static int inode_bit_base = 512;
static int data_bit_base=1536

struct sb {
    long fs_size; //size of file system, in blocks 
    long first_blk; //first block of root directory 
    long bitmap; //size of bitmap, in blocks
}

struct u_fs_file_directory {
    char fname[MAX_FILENAME + 1];
    char fext[MAX_EXTENSION + 1];
    size_t fsize;
    long nStartBlock;
    int flag;
};

struct dir_sb {
    int nNextBlock;
    unsigned int bitmap; //4*8=32 = 512/sizeof(struct inode_map)
};

struct inode_map {
    int inode;
    char fname[MAX_FILENAME + 1];
};

struct u_fs_disk_block {
    size_t size;
    long nNextBlock;
    char data[MAX_DATA_INBLOCK];
};

static int new_block_bit(int fd, int blk);
static int free_block_bit(int fd, int blk);
static int free_inode_bit(int fd, int inode);
static int add_inode(int fd, struct u_fs_file_directory* file_dir);
static int init_dir(int fd, int blk, int self, int parent);
static int add_inode_map(int fd, int blk, struct inode_map* map);
static int delete_inode_map(int fd, int blk, struct inode_map* map, int pre);
static int get_map_count(int fd, int blk);
static int get_inode(int fd, int inode, struct u_fs_file_directory* file_dir);
static int search_file(int fd, int blk, char* fname);
static int parse_path(int fd, const char* path, struct u_fs_file_directory* file_dir);
static void my_filler(int fd, int blk, void* buf, fuse_fill_dir_t filler);
static int add_file(int fd, struct u_fs_file_directory* dir, char* fname);
static void free_file_blocks(int fd, int blk);
static size_t append_data(int fd, int cur_blk, const char* buf, size_t size);

/**
 *这个函数应该查找输入路径以确定它是一个目录还是一个文件。
 *如果是目录，则返回适当的权限。如果是文件，则返回适当的权限和实际大小。
 *这个大小必须是准确的，因为它是用来确定EOF的，因此read不能被调用。
 *成功：返回0，将权限等信息储存在stbuf中
 *失败：返回-ENOENT
 */
static int u_fs_getaddr(const char* path, struct stat* stbuf)
{
    int res = 0;
    memset(stbuf, 0, sizeof(stbuf));
    int fd = open(disk_path, O_RDWR);
    struct u_fs_file_directory dir;
    int inode = parse_path(path, fd,&dir);
    if (inode==-1)
    {
        printf("error:file not found;\n");
        close(fd);
        res=-ENONET;
    }
    else if (dir.flag == 1)
    {
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = dir.fsize;
    }
    else if (dir.flag == 2)
    {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    }
    else
    {
        res = -ENONET;
    }
    return res;
}

/**
 *这个函数应该查找输入路径，确保它是一个目录，然后列出内容。
 *要列出内容，需要使用filler()函数。例如:filler(buf， "."， NULL, 0);将当前目录添加到ls -a生成的清单中
 *通常，您只需要将第二个参数更改为您想要添加到清单中的文件或目录的名称。
 *成功：返回0
 目录不存在或者找不到：返回-ENOENT
 */
static int u_fs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi)
{
    int res = 0;
    (void)offset;
    (void)fi;
    int fd = open(disk_path, O_RDWR);
    struct u_fs_file_directory dir;
    int inode = parse_path(path, fd, &dir);
    if (inode == -1)
    {
        printf("error:%s not found\n",path);
        close(fd);
        res = -ENONET;
    }
    else if (dir.flag != 2)
    {
        printf("error:%s is not a directory\n", path);
        close(fd);
        res = -ENONET;
    }
    else
    {
        my_filler(fd, dir.nStartBlock, buf, filler);
    }
    close(fd);
    return res;
}

/**
 *这个函数应该将新目录添加到根目录，并且应该更新.directories文件
 *成功：返回0
 *名字超过8个字节：返回-ENAMETOOLONG
 *如果该目录不是仅在根目录下：返回-EPERM
 *如果该目录已经存在了：返回-EEXIST
 */
static int u_fs_mkdir(const char* path, mode_t mode)
{
    if (path[0] != '/')
    {
        printf("error:path doesn't start with /\n");
        return -EPERM;
    }
    int fd = open(disk_path, O_RDWR);
    struct sb sblock;
    struct u_fs_file_directory dir;
    if (parse_path(path, fd, &dir) != -1)
    {
        printf("error:path already exists\n");
        return -EEXIST;
    }
    lseek(fd, 0, SEEK_SET);
    nothing = read(fd, &sblock, sizeof(struct sb));
    char* copy_path;
    char* path_flag;
    char file_name[10];
    strcpy(copy_path, path + 1);
    path_flag=strchr(copy_path,'/');
    /*由于只有根目录才有子目录，而子目录是没有子目录的*/
    /*如果有子目录*/
    if (path_flag)
    {
        char* find_path = strchr(path_flag, '\0');
        int length = find_path - path_flag - 1;
        if (length > FILENAME_MAX)
        {
            printf("error:filename too long\n");
            free(copy_path);
            free(find_path);
            free(path_flag);
            close(fd);
            return -ENAMETOOLONG;
        }
        /*类似于"/root/"*/
        else if (length == 0)
        {
            printf("error:do not have filename\n");
            free(copy_path);
            free(find_path);
            free(path_flag);
            close(fd);
            return -EEXIST;
        }
        strncpy(file_name, path_flag+1,length);
        file_name[length] = '\0';
    }
    /*如果没有子目录*/
    else
    {
        if (strlen(copy_path) > FILENAME_MAX)
        {
            printf("error:filename too long\n");
            free(copy_path);
            close(fd);
            return -ENAMETOOLONG;
        }
        strcpy(file_name, copy_path);
        file_name[strlen(copy_path)] = '\0';
    }
    free(copy_path);
    strcpy(dir.fname, file_name);
    strcpy(dir.fext, "");
    dir.flag = 2;
    /*开始找inode块*/
    lseek(fd, inode_bit_base, SEEK_SET);
    unsigned char bit;
    /*在inode节点位图块依次找，直到找到有空的*/
    int start_bit = 0;
    while ((unsigned char)~bit == 0 && start_bit < 1024)
    {
        nothing = read(fd, &bit, 1);
        start_bit++;
    }
    if (start_bit == 1024)
    {
        printf("There is no more place for inode\n");
        inode = -1;
    }
    else
    {
        int flag = 1;
        unsigned char a_bit;
        while (flag <= 8)
        {
            a_bit = bit >> (8 - flag);
            a_bit = a_bit & 1;
            if (!a_bit)break;
            flag++;
        }
        lseek(fd, -1, SEEK_CUR);
        a_bit = 1 << (8 - flag);
        bit = bit | a_bit;
        nothing = write(fd, &bit, 1);
        inode = flag + 8 * start_bit;
    }
    if (inode == -1)
    {
        printf("error:inode can't be created\n");
        close(fd);
        return -EPERM;
    }
    struct inode_map imap;
    strcpy(imap.fname, file_name);
    imap.inode = inode;
    if (add_inode_map(fd, sblock.first_blk, &imap) == -1)
    {
        printf("error:no more place for inde_map\n");
        free_inode_bit(fd, inode);
        close(fd);
        return -EPERM;
    }
    int block= init_dir(fd, -1, inode, 1);
    if (block == -1)
    {
        printf("error:disk is full\n");
        free_inode_bit(fd, inode);
        delete_inode_map(fd, sblock.first_blk, &imap, -1);
        close(fd);
        return -EPERM;
    }
    lseek(fd, inode_base, SEEK_SET);
    lseek(fd, (inode - 1) * sizeof(struct u_fs_file_directory), SEEK_CUR);
    dir.nStartBlock = block;
    nothing== write(fd, &dir, sizeof(struct u_fs_file_directory));
    close(fd);
    return 0;
}

/**
 *删除空目录
 *成功：返回0
 *目录并不为空：返回-ENOTEMPTY
 *目录没有被找到：返回-ENOENT
 *路径并不是一个目录：返回-ENOTDIR
 */
static int u_fs_rmdir(const char* path)
{
    int fd = open(disk_path, O_RDWR);
    struct sb sblock;
    lseek(fd, 0, SEEK_SET);
    nothing = read(fd, &sblock, sizeof(struct sb));
    struct u_fs_file_directory dir;
    int inode = parse_path(fd, path, &dir);
    if (inode == -1)
    {
        printf("error:file doesn't exist\n");
        close(fd);
        return -ENOENT;
    }
    else if(inode==1)
    {
        printf("error:can't delete root dir\n");
        close(fd);
        return -ENOENT;
    }
    if (dir.flag != 2)
    {
        printf("error:%s is not a dir", path);
        close(fd);
        return -ENOTDIR;
    }
    int map_count = get_map_count(fd, dir.nStartBlock);
    if (map_count > 2)
    {
        printf("error:the dictionary is not empty");
        close(fd);
        return -ENOTEMPTY;
    }
    struct inode_map imap;
    strcpy(imap.fname, dir.fname);
    imap.inode = inode;
    delete_inode_map(fd, sblock.first_blk, &imap, -1); //文件夹只存在于根目录中，删除从根目录中删除imap记录
    free_inode_bit(fd, inode);
    free_block_bit(fd, dir.nStartBlock);
    close(fd);
    return 0;
}

/**
 *此函数应将新文件添加到目录，并使用修改后的目录项结构适当地更新.directories文件。
 *以/root/asd为例
 *成功：返回0
 *文件名长度大于8个字节：返回-ENAMETOOLONG
 *文件已经存在：返回-EEXIST
 */
static int u_fs_mknod(const char* path, mode_t mode, dev_t rdev)
{
    (void)mode;
    (void)rdev;
    int fd = open(disk_path, O_RDWR);
    struct u_fs_file_directory dir;
    if (parse_path(fd, path, &dir) != -1)//路径已经存在
    {
        printf("error:can not create new file for file %s exist!\n", path);
        return -EEXIST;
    }
    char* flag_path = strrchr(path, '/');
    char* file_name,*copy_path;
    file_name = malloc(strlen(path) + 1);//file_name为asd
    copy_path = malloc(strlen(path) + 1);
    strcpy(file_name, flag_path + 1);
    if (strlen(file_name) > FILENAME_MAX)
    {
        printf("error:filename is too long\n");
        return -ENAMETOOLONG;
    }
    strncpy(copy_path, path, flag_path - path + 1);//copy_path为/root/
    int length = strlen(copy_path);
    copy_path[length] = '\0';
    int inode = parse_path(fd, copy_path, &dir);
    if (inode == -1)
    {
        printf("error:%s hasn't been created\n",copy_path);
        return -EPERM;
    }
    else if (dir.flag != 2)
    {
        printf("error:%s is not a dir\n", copy_path);
        return -EPERM;
    }
    else if (add_file(fd, &dir, file_name) == -1)
    {
        printf("error:can not create file %s!\n", copy_path);
        res = -EPERM;
    }
    free(copy_path);
    free(file_name);
    close(fd);
    return 0;
}

/**
 *这个函数应该将buf中的数据写入由path表示的文件，从偏移量开始。
 *成功：返回写入数据的大小
 *偏移量太大或失败：返回-EFBIG
 */
static int u_fs_write(const char* path, const char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
    (void)fi;
    int fd = open(disk_path, O_RDWR);
    struct u_fs_file_directory file;
    int inode = parse_path(fd, path, &file);
    if (inode == -1 || file.flag == 0||file.flag==2)
    {
        printf("error:fail\n");
        close(fd);
        return -EFBIG;
    }
    struct u_fs_disk_block block;
    int cur_block = file.nStartBlock;
    if (offset < 0)offset = 0;
    if (offset >= file.fsize)//加在文件末尾
    {
        lseek(fd, (cur_block - 1) * 512, SEEK_SET);
        nothing = read(fd, &block, sizeof(struct u_fs_disk_block));
        while (block.nNextBlock != -1)
        {
            cur_block = block.nNextBlock;
            lseek(fd, (cur_block - 1) * 512, SEEK_SET);
            nothing = read(fd, &block, sizeof(struct u_fs_disk_block));
        }
        int _size = append_data(fd, cur_block, buf, size);
        file.fsize += _size;
        lseek(fd, inode_base, SEEK_SET);
        lseek(fd, (inode - 1) * sizeof(struct u_fs_file_directory), SEEK_CUR);
        nothing = write(fd, &file, sizeof(struct u_fs_file_directory));
        close(fd);
        return _size;
    }
    /*插在文件中间*/
    size_t copy_size = size;
    if (offset + size > file.fsize)
    {
        size = file.fsize - offset;
    }
    lseek(fd, (cur_block - 1) * 512, SEEK_SET);
    nothing = read(fd, &block, sizeof(struct u_fs_disk_block));
    while (offset >= MAX_DATA_INBLOCK)
    {
        if (block.nNextBlock == -1)
        {
            printf("error:wrong offset operation!\n");
            close(fd);
            return 0;
        }
        offset -= MAX_DATA_INBLOCK;
        cur_block = block.nNextBlock;
        lseek(fd, (cur_block - 1) * 512, SEEK_SET);
        nothing = read(fd, &block, sizeof(struct u_fs_disk_block));
    }
    size_t buf_add=0;
    size_t a_size=0;
    while (size > 0)
    {
        int count = block.size - offset;
        if (count > size)count = size;
        memcpy(block.data + offset, buf, count);
        lseek(fd, (cur_block - 1) * 512, SEEK_SET);
        nothing = write(fd, &block, sizeof(struct u_fs_disk_block));
        size -= count;
        buf_add += count;
        a_size += count;
        offset = 0;
        if (block.nNextBlock == -1) break;
        cur_block = block.nNextBlock;
        lseek(fd, (cur_block - 1) * 512, SEEK_SET);
        nothing = read(fd, &block, sizeof(struct u_fs_disk_block));
    }
    if (a_size < copy_size)/*文件内放不下*/
    {
        int _size = append_data(fd, cur_block, buf + buf_add, size - a_size);
        asize += _size;
        file.fsize += _size;
        lseek(fd, inode_base, SEEK_SET);
        lseek(fd, (inode - 1) * sizeof(struct u_fs_file_directory), SEEK_CUR);
        nothing = write(fd, &file, sizeof(struct u_fs_file_directory));
    }
    close(fd);
    return a_size;
}

/**
 *这个函数应该将path表示的文件中的数据读入buf，从偏移量开始。
 *成功：返回写入数据的大小
 *偏移量太大或失败：返回-EISDIR
 */
static int u_fs_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
    (void)fi;
    int fd = open(disk_path, O_RDWR);
    struct u_fs_file_directory file;
    int inode = parse_path(fd, path, &file);
    if (inode == -1 || file.flag == 0 || file.flag == 2)
    {
        printf("error:read fail");
        close(fd);
        return -EISDIR;
    }
    int cur_block = file.nStartBlock;
    if (offset < 0) offset = 0;
    struct u_fs_disk_block block;
    if (offset < file.fsize)
    {
        if (offset + size > file.fsize)
        {
            size = file.fsize - offset;
        }
        size_t buf_add = 0; //buf后接位置
        size_t a_size = 0;
        int cur_block = file.nStartBlock;
        lseek(fd, (cur_block - 1) * 512, SEEK_SET);
        nothing = read(fd, &block, sizeof(struct u_fs_disk_block));
        while (offset >= MAX_DATA_INBLOCK)
        {
            if (block.nNextBlock == -1)
            {
                printf("error:wrong offset operation!\n");
                close(fd);
                return 0;
            }
            offset -= MAX_DATA_INBLOCK;
            cur_block = block.nNextBlock;
            lseek(fd, (cur_block - 1) * 512, SEEK_SET);
            nothing = read(fd, &block, sizeof(struct u_fs_disk_block));
        }
        while (size > 0)
        {
            int count = block.size - offset;
            if (count > size)count = size;
            memcpy(block.data + offset, buf, count);
            size -= count;
            buf_add += count;
            a_size += count;
            offset = 0;
            if (block.nNextBlock == -1) break;
            cur_block = block.nNextBlock;
            lseek(fd, (cur_block - 1) * 512, SEEK_SET);
            nothing = read(fd, &block, sizeof(struct u_fs_disk_block));
        }
        size = a_size;
    }
    else { size = 0; }
    close(fd);
    return size;
}

/**
 *删除一个文件
 *成功：返回0
 *路径是一个目录：返回-EISDIR
 *路径找不到文件：返回-ENOENT
 */
static int u_fs_unlink(const char* path)
{
    int fd == open(disk_path, O_RDWR);
    struct u_fs_file_directory file;//存文件
    struct u_fs_file_directory dir;//存文件的上一级
    int inode = parse_path(fd, path, &file);
    if (inode == -1 || file.flag == 0)
    {
        printf("error:%s is not a file\n");
        close(fd);
        return -ENOENT;
    }
    else if (file.flag == 2)
    {
        printf("error:%s is a dictionary\n");
        close(fd);
        return -EISDIR;
    }
    char* flag_path = strrchr(path, '/');
    char* file_name, * copy_path;
    file_name = malloc(strlen(path) + 1);
    copy_path = malloc(strlen(path) + 1);
    strcpy(file_name, flag_path + 1);
    strncpy(copy_path, path, flag_path - path + 1);
    int length = strlen(copy_path);
    copy_path[length] = '\0';
    struct inode_map imap;
    imap.inode = inode;
    strcpy(imap.fname, file.fname);
    parse_path(fd, copy_path, &dir);
    delete_inode_map(fd, dir.nStartBlock, &imap, -1);
    free_inode_bit(fd, inode);
    free_file_blocks(fd, file.nStartBlock);
    close(fd);
    return 0;
}

static int u_fs_open(const char* path, struct fuse_file_info* fi)
{
    (void)path;
    (void)fi;
    return 0;
}

static int u_fs_flush(const char* path, struct fuse_file_info* fi)
{
    (void)path;
    (void)fi;
    return 0;
}

static int u_fs_truncate(const char* path, off_t size)
{
    (void)path;
    (void)size;
    return 0;
}

static struct fuse_operations u_fs_oper = {
    .getattr = u_fs_getattr,
    .readdir = u_fs_readdir,
    .mkdir = u_fs_mkdir,
    .rmdir = u_fs_rmdir,
    .mknod = u_fs_mknod,
    .write = u_fs_write,
    .read = u_fs_read,
    .unlink = u_fs_unlink,
    .open = u_fs_open,
    .flush = u_fs_flush,
    .truncate = u_fs_truncate,
};

/**
 * 初始化
 */
static int init(void)
{
    int fd = open(disk_path, O_RDWR);
    if (fd == -1)
    {
        printf("error:can not open file %s!\n", disk_path);
        exit(1);
    }
    struct sb sblock;
    sblock.fs_size = BLOCK_MAX;
    int inode_bitmap_count = sizeof(struct u_fs_file_directory) * INODE_MAX;/*inode节点块总位数*/
    int inode_data_count = inode_bitmap_count / 512;/*inode节点块个数*/
    if (inode_bitmap_count != 0)
    {
        inode_data_count++;
    }
    sblock.first_blk = 1 + 2 + 5 +inode_data_count+ 1;
    sblock.bitmap = BLOCK_MAX;
    nothing = write(fd, &sblock, sizeof(struct sb));
    int count = 1;
    while (count < sblock.first_blk)
    {
        new_block_bit(fd, count);
        count++;
    }
    //设置第一个inode 根目录
    struct u_fs_file_directory root_dir;
    strcpy(root_dir.fname, ".");
    strcpy(root_dir.fext, "");
    root_dir.fsize = 0;
    root_dir.nStartBlock = sblock.first_blk;
    root_dir.flag = 2;
    int root_inode = add_inode(fd, &root_dir);
    if (root_inode == -1)
    {
        printf("error:initialize error!\n");
        exit(1);
    }

    init_dir(fd, root_dir.nStartBlock, root_inode, root_inode);
    close(fd);
    return 0;
}

/**
 *获取一个inode
 *通过给定的inode号得到inode节点块的信息并返回0
 *信息储存在file_dir中
 */
static int get_inode(int fd, int inode, struct u_fs_file_directory* file_dir)
{
    lseek(fd, inode_base, SEEK_SET);//重新指派起始位置
    lseek(fd, (inode - 1) * sizeof(struct u_fs_file_directory), SEEK_CUR);
    nothing = read(fd, file_dir, sizeof(struct u_fs_file_directory));
    return 0;
}

/**
 *分析路径
 *输入路径和文件描述符
 *这里的路径有可能只是目录，也有可能包含文件
 *以/root/asd或者/asd为例
 *成功：返回inode以及inode节点块（储存在file_dir中）
 *失败：返回-1 
 */
static int parse_path(const char* path, int fd, struct u_fs_file_directory* file_dir)
{
    if (path[0] != '/')
    {
        return -1;
    }
    /*从根目录开始搜索*/
    int inode = 1;
    get_node(fd, inode, file_dir);//现在file_dir为根目录
    char *copy_path;
    copy_path = malloc(strlen(path) + 1);
    strcpy(copy_path, path + 1);
    while (strcmp(copy_path, "\0") != 0)
    {
        char *find_path = strchr(copy_path, '/');
        char file_name[256];
        if (find_path)/*还有两级文件*/
        {
            strncpy(file_name, copy_path, find_path - copy_path);/*file_name为root*/
            file_name[find_path - copy_path] = '\0';
            strcpy(copy_path, find_path + 1);/*copy_path为asd*/
        }
        else/*还有一级文件*/
        {
            strcpy(file_name, copy_path);/*file_name为asd*/
            strcpy(copy_path, "\0");/*copy_path为空*/
        }
        inode = search_file(fd, file_dir->nStartBlock, file_name);
        if(inode==-1)
        {
            free(file_name);
            free(find_path);
            free(copy_path);
            return -1;
        }
        else
        {
            get_inode(fd, inode, file_dir);
        }
    }
    free(file_name);
    free(find_path);
    free(copy_path);
    return inode;
}

/**
 *根据块号在文件夹中寻找文件
 *有可能是目录或文件
 *成功：返回file_name所在文件inode
 *失败：返回-1
 */
static int search_file(int fd, int block, char* file_name)
{
    struct dir_sb dir;
    lseek(fd, (block - 1) * 512, SEEK_SET);
    nothing = read(fd, &dir, sizeof(struct dir_sb));
    int flag = 2;
    unsigned int map;
    struct inode_map imap;
    while (flag <= 2)
    {
        map = dir_sb.bitmap >> (32 - flag);
        map = map & 1;
        if (map)
        {
            lseek(fd, (block - 1) * 512, SEEK_SET);
            lseek(fd, (flag - 1) * 16, SEEK_CUR);
            nothing = read(fd, &imap, sizeof(inode_map);
            if (strcmp(imap.fname , file_name))
            {
                return imap.inode;
            }
        }
        flag++;
    }
    if (dir.nNextBlock != -1)
    {
        return search_file(fd, dir.nNextBlock, file_name);
    }
    return -1;
}

/**
 *filler填充目录的子文件和子文件夹
 *递归目录的所有数据块进行填充
 *意思就是说你写的文件系统虽然有目录间的关系了，但是运行linux命令ls -a的时候是不会显示的
 *因此我们需要使用到filler函数，这样ls -a才能显示出来
 *例如/root目录下有a1，a2，a3三个文件，一番操作后在/root下ls是没有结果的，只有用了filler才有结果
 */
static void my_filler(int fd, int block, void* buf, fuse_fill_dir_t filler)
{
    if (block == -1) return;
    struct dir_sb dir;
    lseek(fd, (block - 1) * 512, SEEK_SET);
    nothing = read(fd, &dir, sizeof(struct dir_sb));
    int flag = 2;
    unsigned int map;
    struct inode_map imap;
    while (flag <= 2)
    {
        map = dir_sb.bitmap >> (32 - flag);
        map = map & 1;
        if (map)
        {
            lseek(fd, (block - 1) * 512, SEEK_SET);
            lseek(fd, (flag - 1) * 16, SEEK_CUR);
            nothing = read(fd, &imap, sizeof(struct inode_map);
            filler(buf, imap.fname, NULL, 0);
        }
        flag++;
    }
    if (dir.nNextBlock != -1)
    {
        my_filler(fd, block, buf, filler);
    }
    return;
}

/**
 *在block中添加map信息
 *block所属的是一个文件夹数据
 *map中有可能是文件夹或者文件
 *成功：返回0
 *失败：返回-1
 */
static int add_inode_map(int fd, int block, struct inode_map* map)
{
    struct dir_sb sb;
    lseek(fd, (block - 1) * 512, SEEK_SET);
    nothing = read(fd, &sb, sizeof(struct dir_sb));
    int flag = 2;
    unsigned int bit;
    while (flag <= 32)
    {
        bit = sb.bitmap >> (32 - flag);
        bit = bit & 1;
        if (!bit)break;
        flag++;
    }
    if (flag == 33&&sb.nNextBlock!=-1)//在下一块继续寻找空间
    {
        return add_inode_map(fd, sb.nNextBlock, map);
    }
    else if (flag == 33 && sb.nNextBlock == -1)//创建一个新的空间来放数据
    {
        int nblock = new_block_bit(fd, -1);
        if (nblock == -1)
        {
            return -1;
        }
        sb.nNextBlock = nblock;
        lseek(fd, (block - 1) * 512, SEEK_SET);
        nothing = write(fd, &sb, struct(dir_sb));
        struct dir_sb new_sb;
        new_sb.nNextBlock = -1;
        new_sb.bitmap = 1 << 31;
        lseek(fd, (nblock - 1) * 512, SEEK_SET);
        nothing = write(fd, &new_sb, struct(dir_sb));
        return add_inode_map(fd, nblock, map);
    }
    else if (flag <= 32)//在本快中寻找空间
    {
        lseek(fd, (block - 1) * 512, SEEK_SET);
        lseek(fd, (flag - 1) * 16, SEEK_CUR);
        nothing = write(fd, map, sizeof(struct inode_map));
        lseek(fd, (block - 1) * 512, SEEK_SET);
        bit = 1 << (32 - flag);
        sb.bitmap = sb.bitmap | bit;
        nothing = write(fd, sb, sizeof(struct dir_sb));
    }
    return 0;
}

/**
 *在磁盘块位图中占用一个inode bitmap位
 *block为-1说明是找第一个空闲位
 *否则就是改写block所在的bitmap位
 *成功：返回新的block的编号
 *失败：返回-1
 */
static int new_block_bit(int fd, int block)
{
    if (block > BLOCK_MAX)return -1;
    if (block == -1)
    {
        /*去磁盘块位图找空位*/
        unsigned char bit;
        int start_bit = 0;
        lseek(fd, data_bit_base, SEEK_SET);
        nothing = write(fd, &bit, 1);
        while ((unsigned char)~bit == 0 && start_bit < 2560)
        {
            nothing = read(fd, &bit, 1);
            start_bit++;
        }
        if (start_bit == 2560)
        {
            printf("There is no more place for data_map\n");
            return -1;
        }
        else
        {
            int flag = 1;
            unsigned char a_bit;
            while (flag <= 8)
            {
                a_bit = bit >> (8 - flag);
                a_bit = a_bit & 1;
                if (!a_bit)break;
                flag++;
            }
            lseek(fd, -1, SEEK_CUR);
            a_bit = 1 << (8 - flag);
            bit = bit | a_bit;
            nothing = write(fd, &bit, 1);
            return flag + 8 * start_bit;
        }
    }
    else
    {
        int start_bit = block / 8;
        int flag = block % 8;
        if (flag == 0)
        {
            start_bit--;
            flag = 8;
        }
        lseek(fd, data_bit_base, SEEK_SET);
        lseek(fd, start_bit, SEEK_CUR);
        unsigned char bit;
        nothing = read(fd, &bit, 1);
        unsigned char a_bit = bit >> (8 - flag);
        a_bit = a_bit & 1;
        if (a_bit)return -1;
        lseek(fd, -1, SEEK_CUR);
        a_bit = 1 << (8 - flag);
        bit = bit | a_bit;
        nothing = write(fd, &bit, 1);
        return block;
    }
}

/**
 *从inode节点位图中回收inode节点
 *成功：返回inode值
 *失败：返回-1
 */
static int free_inode_bit(int fd, int inode)
{
    if (inode > INODE_MAX) 
    {
        return -1;
    }
    int start_bit = inode / 8;
    int flag = inode % 8;
    if (flag == 0)
    {
        start_bit--;
        flag = 8;
    }
    lseek(fd, inode_bit_base, SEEK_SET);
    lseek(fd, start_bit, SEEK_CUR);
    unsigned char bit;
    nothing = read(fd, &bit, 1);
    unsigned char a_bit = bit >> (8 - flag);
    a_bit = a_bit & 1;
    if (a_bit)return -1;
    lseek(fd, -1, SEEK_CUR);
    a_bit = 1 << (8 - flag);
    a_bit = ~a_bit;
    bit = bit | a_bit;
    nothing = write(fd, &bit, 1);
    return inode;
}

/**
 *初始化一个文件夹
 *要在该文件夹的数据块中加入自己的数据和父inode的数据
 *成功：返回块号
 */
static int init_dir(int fd, int block, int inode, int parent_inode)
{
    block = new_block_bit(fd, block);
    if (block == -1)return -1;
    struct dir_sb sb;
    sb.nNextBlock = -1;
    sb.bitmap = 1 << 31;
    lseek(fd, (block - 1) * 512, SEEK_SET);
    nothing= write(fd, &sb, sizeof(struct dir_sb));
    struct inode_map imap;
    strcpy(imap.fname, ".");
    imap.inode = inode;
    add_inode_map(fd, block, &imap);
    strcpy(imap.fname, "..");
    imap.inode = parent_inode;
    add_inode_map(fd, block, &imap);
    return block;
}

/**
 *删除一个子文件或者子文件夹inode
 *给定目录的block，删除imap
 *成功：返回0
 *失败：返回-1
 */
static int delete_inode_map(int fd, int blk, struct inode_map* map, int pre)
{
    struct dir_sb sb;
    lseek(fd, (block - 1) * 512, SEEK_SET);
    nothing = read(fd, &sb, sizeof(dir_sb));
    int flag = 2;
    unsigned int bit;
    int bit_count = 0;
    while (flag <= 32)
    {
        bit = sb.bitmap >> (32 - flag);
        bit = bit & 1;
        if (bit)bit_count++;
        flag++;
    }
    flag = 2;
    while (flag <= 32)
    {
        bit = sb.bitmap >> (32 - flag);
        bit = bit & 1;
        if (bit)
        {
            lseek(fd, (block - 1) * 512, SEEK_SET);
            lseek(fd, (flag - 1) * 16, SEEK_CUR);
            struct inode_map imap;
            nothing = read(fd, &imap, sizeof(struct inode_map));
            if (strcmp(map->fname, imap.fname) == 0 && map->inode == imap.inode)
                break;
        }
        flag++;
    }
    /*找到了*/
    if (flag <= 32)
    {
        bit = 1 << (32 - flag);
        bit = ~bit;
        sb.bitmap = sb.bitmap & bit;
        bit_count--;
        if (bit_count == 0 && pre != -1)//无记录，删除
        {
            free_block_bit(fd, block);
            struct dir_sb a_sb;
            lseek(fd, (pre - 1) * 512, SEEK_SET);
            nothing= read(fd, &a_sb, sizeof(struct dir_sb));
            a_sb.nNextBlock = sb.nNextBlock;
            lseek(fd, (pre - 1) * 512, SEEK_SET);
            nothing = write(fd, &a_sb, sizeof(struct dir_sb));
        }
        return 0;
    }
    else
    {
        return delete_inode_map(fd, sb.nNextBlock, map, block);
    }
    return -1;
}

/**
 *得到目录下面的文件和子目录数
 *给定目录的块号block
 *成功：返回总数
 */
static int get_map_count(int fd, int block)
{
    if (block == -1)return 0;
    int count = 0;
    lseek(fd, (block - 1) * 512, SEEK_SET);
    struct dir_sb, sb;
    nothing = read(fd, &sb, sizeof(dir_sb));
    unsigned int bit;
    int flag = 2;
    while (flag <= 32)
    {
        bit = sb.bitmap >> (32 - flag);
        bit = bit & 1;
        if (bit)count++;
        flag++;
    }
    return count + get_map_count(fd, sb.nNextBlock);
}

/**
 *新建一个文件
 *dir的目录内
 *成功：返回0
 *失败：返回-1
 */
static int add_file(int fd, struct u_fs_file_directory* dir, char* filename)
{
    struct u_fs_disk_block file;
    file.size = 0;
    file.nNextBlock = -1;
    int block = new_block_bit(fd, -1);
    if (block == -1)
    {
        printf("no more place for block\n");
        return -1;
    }
    lseek(fd, (block - 1) * 512, SEEK_SET);
    nothing = write(fd, &file, sizeof(u_fs_disk_block));
    struct u_fs_file_directory newfile;
    strcpy(newfile.fname, filename);
    strcpy(newfile.fext, "");
    newfile.flag = 1;
    newfile.fsize = 0;
    newfile.nStartBlock = block;
    int inode = add_inode(fd, &newfile);
    if(inode==-1)
    {
        printf("can't create inode\n");
        free_block_bit(fd, block);
        return -1;
    }
    struct inode_map imap;
    strcpy(map.fname, file_name);
    imap.inode = inode;
    if (add_inode_map(fd, dir.nStartBlock, &map) == -1)
    {
        printf("error:can't create inode_map");
        free_block_bit(fd, block);
        free_inode_bit(fd, inode);
        return -1;
    }
    return 0;
}

/**
 *回收数据块
 *block：要回收的数据块
 *在磁盘块位图中回收
 *数据块成功回收：返回block
 *数据块本身就是空闲的或者回收失败：返回-1
 */
static free_block_bit(int fd, int block)
{
    if (block > BLOCK_MAX)
    {
        return -1;
    }
    int start_block = block / 8;
    int flag = block % 8;
    if (flag == 0)
    {
        start_block--;
        flag = 8;
    }
    lseek(fd, data_bit_base, SEEK_SET);
    lseek(fd, start_block, SEEK_CUR);
    unsigned char bit;
    nothing = read(fd, &bit, 1);
    unsigned char a_bit = bit >> (8 - flag);
    a_bit = a_bit & 1;
    if (a_bit)return -1;
    lseek(fd, -1, SEEK_CUR);
    a_bit = 1 << (8 - flag);
    a_bit = ~a_bit;
    bit = bit | a_bit;
    nothing = write(fd, &bit, 1);
    return block;
}
/**
 *向文件后面添加数据
 *成功：返回添加数据的大小
 *失败：返回0
 */
static size_t append_data(int fd, int cur_block, const char* buf, size_t size)
{
    if (cur_block == -1 || size <= 0)return 0;
    struct u_fs_disk_block block;
    lseek(fd, (cur_block - 1) * 512, SEEK_SET);
    nothing = read(fd, &block, sizeof(struct u_fs_disk_block));
    size_t count = MAX_DATA_INBLOCK - block.size;
    if (count > size)count = size;
    memcpy(block.data + block.size, buf, count);
    /*有可能此数据块不够大，需要多加一块*/
    size -= count;
    buf += count;
    block.size += count;
    if(size != 0)
    {
        int n_block = new_block_bit(fd, -1);
        if (n_block != -1)
        {
            block.nNextBlock = n_block;
            struct u_fs_disk_block n_block;
            n_block.nNextBlock = -1;
            n_block.size = 0;
            lseek(fd, n__block - 1) * 512, SEEK_SET);
            nothing = write(fd, &n_block, sizeof(struct u_fs_disk_block));
        }
    }
    lseek(fd, (cur_block - 1) * 512, SEEK_SET);
    nothing = write(fd, &block, sizeof(struct u_fs_disk_block));
    return count + append_data(fd, block.nNextBlock, buf, size);
}

/**
 * 回收某文件所占得所有数据块
 * 当删除一个文件时要对其占用的所有数据块进行回收
 * 无返回
 */
static void free_file_blocks(int fd, int block)
{
    if (block == 1)return;//根目录不能释放
    free_block_bit(fd, block);
    struct u_fs_disk_block file_block;
    lseek(fd, (block - 1) * 512, SEEK_SET);
    nothing = read(fd, &file_block, sizeof(struct u_fs_disk_block));
    if (file_block.nNextBlock != -1)
        free_file_blocks(fd, file_block.nNextBlock);
    return;
}

/**
 * 增加一个inode保存file_dir 节点信息
 * 成功：return保存所在的inode序号
 * 失败：return -1
 */
static int add_inode(int fd, struct u_fs_file_directory* file_dir)
{
    int inode = new_block_bit(fd, -1);
    lseek(fd, inode_base, SEEK_SET);
    lseek(fd, (inode - 1) * sizeof(struct u_fs_file_directory), SEEK_CUR);
    nothing = write(fd, file_dir, sizeof(struct u_fs_file_directory));
    return inode;
}

int main(int argc, char* argv[])
{
    init();
    return fuse_main(argc, argv, &u_fs_oper, NULL);
}