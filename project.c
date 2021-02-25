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
 *�������Ӧ�ò�������·����ȷ������һ��Ŀ¼����һ���ļ���
 *�����Ŀ¼���򷵻��ʵ���Ȩ�ޡ�������ļ����򷵻��ʵ���Ȩ�޺�ʵ�ʴ�С��
 *�����С������׼ȷ�ģ���Ϊ��������ȷ��EOF�ģ����read���ܱ����á�
 *�ɹ�������0����Ȩ�޵���Ϣ������stbuf��
 *ʧ�ܣ�����-ENOENT
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
 *�������Ӧ�ò�������·����ȷ������һ��Ŀ¼��Ȼ���г����ݡ�
 *Ҫ�г����ݣ���Ҫʹ��filler()����������:filler(buf�� "."�� NULL, 0);����ǰĿ¼��ӵ�ls -a���ɵ��嵥��
 *ͨ������ֻ��Ҫ���ڶ�����������Ϊ����Ҫ��ӵ��嵥�е��ļ���Ŀ¼�����ơ�
 *�ɹ�������0
 Ŀ¼�����ڻ����Ҳ���������-ENOENT
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
 *�������Ӧ�ý���Ŀ¼��ӵ���Ŀ¼������Ӧ�ø���.directories�ļ�
 *�ɹ�������0
 *���ֳ���8���ֽڣ�����-ENAMETOOLONG
 *�����Ŀ¼���ǽ��ڸ�Ŀ¼�£�����-EPERM
 *�����Ŀ¼�Ѿ������ˣ�����-EEXIST
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
    /*����ֻ�и�Ŀ¼������Ŀ¼������Ŀ¼��û����Ŀ¼��*/
    /*�������Ŀ¼*/
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
        /*������"/root/"*/
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
    /*���û����Ŀ¼*/
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
    /*��ʼ��inode��*/
    lseek(fd, inode_bit_base, SEEK_SET);
    unsigned char bit;
    /*��inode�ڵ�λͼ�������ң�ֱ���ҵ��пյ�*/
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
 *ɾ����Ŀ¼
 *�ɹ�������0
 *Ŀ¼����Ϊ�գ�����-ENOTEMPTY
 *Ŀ¼û�б��ҵ�������-ENOENT
 *·��������һ��Ŀ¼������-ENOTDIR
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
    delete_inode_map(fd, sblock.first_blk, &imap, -1); //�ļ���ֻ�����ڸ�Ŀ¼�У�ɾ���Ӹ�Ŀ¼��ɾ��imap��¼
    free_inode_bit(fd, inode);
    free_block_bit(fd, dir.nStartBlock);
    close(fd);
    return 0;
}

/**
 *�˺���Ӧ�����ļ���ӵ�Ŀ¼����ʹ���޸ĺ��Ŀ¼��ṹ�ʵ��ظ���.directories�ļ���
 *��/root/asdΪ��
 *�ɹ�������0
 *�ļ������ȴ���8���ֽڣ�����-ENAMETOOLONG
 *�ļ��Ѿ����ڣ�����-EEXIST
 */
static int u_fs_mknod(const char* path, mode_t mode, dev_t rdev)
{
    (void)mode;
    (void)rdev;
    int fd = open(disk_path, O_RDWR);
    struct u_fs_file_directory dir;
    if (parse_path(fd, path, &dir) != -1)//·���Ѿ�����
    {
        printf("error:can not create new file for file %s exist!\n", path);
        return -EEXIST;
    }
    char* flag_path = strrchr(path, '/');
    char* file_name,*copy_path;
    file_name = malloc(strlen(path) + 1);//file_nameΪasd
    copy_path = malloc(strlen(path) + 1);
    strcpy(file_name, flag_path + 1);
    if (strlen(file_name) > FILENAME_MAX)
    {
        printf("error:filename is too long\n");
        return -ENAMETOOLONG;
    }
    strncpy(copy_path, path, flag_path - path + 1);//copy_pathΪ/root/
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
 *�������Ӧ�ý�buf�е�����д����path��ʾ���ļ�����ƫ������ʼ��
 *�ɹ�������д�����ݵĴ�С
 *ƫ����̫���ʧ�ܣ�����-EFBIG
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
    if (offset >= file.fsize)//�����ļ�ĩβ
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
    /*�����ļ��м�*/
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
    if (a_size < copy_size)/*�ļ��ڷŲ���*/
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
 *�������Ӧ�ý�path��ʾ���ļ��е����ݶ���buf����ƫ������ʼ��
 *�ɹ�������д�����ݵĴ�С
 *ƫ����̫���ʧ�ܣ�����-EISDIR
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
        size_t buf_add = 0; //buf���λ��
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
 *ɾ��һ���ļ�
 *�ɹ�������0
 *·����һ��Ŀ¼������-EISDIR
 *·���Ҳ����ļ�������-ENOENT
 */
static int u_fs_unlink(const char* path)
{
    int fd == open(disk_path, O_RDWR);
    struct u_fs_file_directory file;//���ļ�
    struct u_fs_file_directory dir;//���ļ�����һ��
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
 * ��ʼ��
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
    int inode_bitmap_count = sizeof(struct u_fs_file_directory) * INODE_MAX;/*inode�ڵ����λ��*/
    int inode_data_count = inode_bitmap_count / 512;/*inode�ڵ�����*/
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
    //���õ�һ��inode ��Ŀ¼
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
 *��ȡһ��inode
 *ͨ��������inode�ŵõ�inode�ڵ�����Ϣ������0
 *��Ϣ������file_dir��
 */
static int get_inode(int fd, int inode, struct u_fs_file_directory* file_dir)
{
    lseek(fd, inode_base, SEEK_SET);//����ָ����ʼλ��
    lseek(fd, (inode - 1) * sizeof(struct u_fs_file_directory), SEEK_CUR);
    nothing = read(fd, file_dir, sizeof(struct u_fs_file_directory));
    return 0;
}

/**
 *����·��
 *����·�����ļ�������
 *�����·���п���ֻ��Ŀ¼��Ҳ�п��ܰ����ļ�
 *��/root/asd����/asdΪ��
 *�ɹ�������inode�Լ�inode�ڵ�飨������file_dir�У�
 *ʧ�ܣ�����-1 
 */
static int parse_path(const char* path, int fd, struct u_fs_file_directory* file_dir)
{
    if (path[0] != '/')
    {
        return -1;
    }
    /*�Ӹ�Ŀ¼��ʼ����*/
    int inode = 1;
    get_node(fd, inode, file_dir);//����file_dirΪ��Ŀ¼
    char *copy_path;
    copy_path = malloc(strlen(path) + 1);
    strcpy(copy_path, path + 1);
    while (strcmp(copy_path, "\0") != 0)
    {
        char *find_path = strchr(copy_path, '/');
        char file_name[256];
        if (find_path)/*���������ļ�*/
        {
            strncpy(file_name, copy_path, find_path - copy_path);/*file_nameΪroot*/
            file_name[find_path - copy_path] = '\0';
            strcpy(copy_path, find_path + 1);/*copy_pathΪasd*/
        }
        else/*����һ���ļ�*/
        {
            strcpy(file_name, copy_path);/*file_nameΪasd*/
            strcpy(copy_path, "\0");/*copy_pathΪ��*/
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
 *���ݿ�����ļ�����Ѱ���ļ�
 *�п�����Ŀ¼���ļ�
 *�ɹ�������file_name�����ļ�inode
 *ʧ�ܣ�����-1
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
 *filler���Ŀ¼�����ļ������ļ���
 *�ݹ�Ŀ¼���������ݿ�������
 *��˼����˵��д���ļ�ϵͳ��Ȼ��Ŀ¼��Ĺ�ϵ�ˣ���������linux����ls -a��ʱ���ǲ�����ʾ��
 *���������Ҫʹ�õ�filler����������ls -a������ʾ����
 *����/rootĿ¼����a1��a2��a3�����ļ���һ����������/root��ls��û�н���ģ�ֻ������filler���н��
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
 *��block�����map��Ϣ
 *block��������һ���ļ�������
 *map���п������ļ��л����ļ�
 *�ɹ�������0
 *ʧ�ܣ�����-1
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
    if (flag == 33&&sb.nNextBlock!=-1)//����һ�����Ѱ�ҿռ�
    {
        return add_inode_map(fd, sb.nNextBlock, map);
    }
    else if (flag == 33 && sb.nNextBlock == -1)//����һ���µĿռ���������
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
    else if (flag <= 32)//�ڱ�����Ѱ�ҿռ�
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
 *�ڴ��̿�λͼ��ռ��һ��inode bitmapλ
 *blockΪ-1˵�����ҵ�һ������λ
 *������Ǹ�дblock���ڵ�bitmapλ
 *�ɹ��������µ�block�ı��
 *ʧ�ܣ�����-1
 */
static int new_block_bit(int fd, int block)
{
    if (block > BLOCK_MAX)return -1;
    if (block == -1)
    {
        /*ȥ���̿�λͼ�ҿ�λ*/
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
 *��inode�ڵ�λͼ�л���inode�ڵ�
 *�ɹ�������inodeֵ
 *ʧ�ܣ�����-1
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
 *��ʼ��һ���ļ���
 *Ҫ�ڸ��ļ��е����ݿ��м����Լ������ݺ͸�inode������
 *�ɹ������ؿ��
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
 *ɾ��һ�����ļ��������ļ���inode
 *����Ŀ¼��block��ɾ��imap
 *�ɹ�������0
 *ʧ�ܣ�����-1
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
    /*�ҵ���*/
    if (flag <= 32)
    {
        bit = 1 << (32 - flag);
        bit = ~bit;
        sb.bitmap = sb.bitmap & bit;
        bit_count--;
        if (bit_count == 0 && pre != -1)//�޼�¼��ɾ��
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
 *�õ�Ŀ¼������ļ�����Ŀ¼��
 *����Ŀ¼�Ŀ��block
 *�ɹ�����������
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
 *�½�һ���ļ�
 *dir��Ŀ¼��
 *�ɹ�������0
 *ʧ�ܣ�����-1
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
 *�������ݿ�
 *block��Ҫ���յ����ݿ�
 *�ڴ��̿�λͼ�л���
 *���ݿ�ɹ����գ�����block
 *���ݿ鱾����ǿ��еĻ��߻���ʧ�ܣ�����-1
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
 *���ļ������������
 *�ɹ�������������ݵĴ�С
 *ʧ�ܣ�����0
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
    /*�п��ܴ����ݿ鲻������Ҫ���һ��*/
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
 * ����ĳ�ļ���ռ���������ݿ�
 * ��ɾ��һ���ļ�ʱҪ����ռ�õ��������ݿ���л���
 * �޷���
 */
static void free_file_blocks(int fd, int block)
{
    if (block == 1)return;//��Ŀ¼�����ͷ�
    free_block_bit(fd, block);
    struct u_fs_disk_block file_block;
    lseek(fd, (block - 1) * 512, SEEK_SET);
    nothing = read(fd, &file_block, sizeof(struct u_fs_disk_block));
    if (file_block.nNextBlock != -1)
        free_file_blocks(fd, file_block.nNextBlock);
    return;
}

/**
 * ����һ��inode����file_dir �ڵ���Ϣ
 * �ɹ���return�������ڵ�inode���
 * ʧ�ܣ�return -1
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