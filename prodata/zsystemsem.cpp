#include "zsystemsem.h"
#include <openssl/sha.h>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <fcntl.h>
#include "zfile.h"
#include "sem/syssem.h"

ZSystemSem::ZSystemSem():m_unix_key(-1),m_sem(-1),m_initVal(0),m_createdFile(false),m_createdSem(false)
{
}


inline string ZSystemSem::makeKeyFileName() const
{
    if(m_key.empty())
        return string();
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, m_key.c_str(), m_key.size());
    SHA256_Final(hash, &sha256);

    stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << hex << setw(2) << setfill('0') << (int)hash[i];
    }
    string retString = "/tmp/zipc_systemsem_" + ss.str();
    return retString;
}

int  ZSystemSem::createUnixKeyFile(const string &fileName)
{
    if(ZFile::exists(fileName))
        return 0;
    int fd = open(fileName.c_str(), O_EXCL | O_CREAT | O_RDWR, 0640);

    if(-1 ==fd)
    {
        if(errno == EEXIST)
            return 0;
        return -1;
    }
    else
    {
        close(fd);
    }
    return 1;

}

key_t ZSystemSem::handle(AccessMode mode)
{
    if(-1 != m_unix_key)
        return m_unix_key;
    if(m_key.empty())
    {
        zprintf1("ZSystemSem m_key empty!\n");
        return -1;
    }
    int built = createUnixKeyFile(m_fileName);
    if(-1 == built)
    {
        zprintf1("unable to make key %s!\n", m_key.c_str());
        return -1;
    }

    m_createdFile = (1 == built);

    m_unix_key = ftok(m_fileName.c_str(), 'Z');
    if(-1 == m_unix_key)
    {
        zprintf1("ftok failed %s!\n", m_fileName.c_str());
        return -1;
    }
    m_sem = semget(m_unix_key, 1, 0600 | IPC_CREAT | IPC_EXCL);
    if(-1 == m_sem)
    {
        if(errno == EEXIST)
            m_sem = semget(m_unix_key, 1, 0600 | IPC_CREAT);
        if(-1 == m_sem)
        {
            zprintf1("semget m_unix_key %d error!\n", m_unix_key);
            cleanHandle();    //缺少清理函数
            return -1;
        }
        if(mode == Create)
        {
            m_createdSem = true;
            m_createdFile = true;
        }
    }
    else
    {
        m_createdFile = true;
        m_createdSem = true;
    }

    if (m_createdSem && m_initVal >= 0) {
        semun init_op;
        init_op.val = m_initVal;
        if (-1 == semctl(m_sem, 0, SETVAL, init_op)) {
            zprintf1("init sem %d error!\n", m_unix_key);
            perror("Sem init");
            cleanHandle();
            return -1;
        }
    }

    return m_unix_key;
}

void ZSystemSem::cleanHandle()
{
    m_unix_key = -1;
    if(m_createdFile)
    {
        ZFile::remove(m_fileName);
        m_createdFile = false;
    }

    if(m_createdSem)
    {
        if(-1 != m_sem)
        {
            if(-1 == semctl(m_sem, 0, IPC_RMID, 0))
            {
                zprintf1("rm msem %d error!\n", m_sem);
            }
            m_sem = -1;
        }
        m_createdSem = false;
    }

}

void ZSystemSem::setKey(const string &key, int initVal, AccessMode mode)
{
    if(key == m_key && mode == Open)
    {
        zprintf1("% is have!\n", key.c_str());
        return;
    }
    if(key == m_key && mode == Create && m_createdSem && m_createdFile)
    {
        m_initVal = initVal;
        m_unix_key = -1;
        handle(mode);
        return;
    }

    cleanHandle();

    m_key = key;
    m_initVal = initVal;
    m_fileName = makeKeyFileName();
    handle(mode);
}

string ZSystemSem::key() const
{
    return m_key;
}

bool ZSystemSem::modifySemaphore(int count)
{
    if(-1 == handle())
    {
        return false;
    }
    struct sembuf operation;
    operation.sem_num = 0;
    operation.sem_op = count;
    operation.sem_flg = SEM_UNDO;

    // register int res;
    int res;
    EINTR_LOOP(res, semop(m_sem, &operation, 1));
    if( -1 == res)
    {
        if(errno == EINVAL || errno == EIDRM)
        {
            m_sem = -1;
            cleanHandle();
            handle();
            return modifySemaphore(count);
        }
        zprintf1("semop m_sem %d error!\n", m_sem);
        return false;
    }
    return true;
}

ZSystemSem::SemOpTimeState ZSystemSem::modifySemaphore(int count, int ms)
{
    if(-1 == handle())
    {
        return PError;
    }
    struct sembuf operation;
    operation.sem_num = 0;
    operation.sem_op = count;
    operation.sem_flg = SEM_UNDO;

    struct timespec timeout;
    timeout.tv_sec  = (ms / 1000);
    timeout.tv_nsec = (ms - timeout.tv_sec * 1000L) * 1000000L;

    // register int res;
    int res;
    EINTR_LOOP(res, semtimedop(m_sem, &operation, 1, &timeout));
    if( -1 == res)
    {
        if(errno == EINVAL || errno == EIDRM)
        {
            m_sem = -1;
            cleanHandle();
            handle();
            return modifySemaphore(count, ms);
        }
        else if(errno == ETIMEDOUT)
            return PTimeOver;
        zprintf1("semop m_sem %d error!\n", m_sem);
        return PError;
    }
    return Pok;
}

bool ZSystemSem::acquire()
{
   return modifySemaphore(-1);
}

bool ZSystemSem::release(int n)
{
    if(n == 0)
        return true;
    if(n < 0)
    {
        zprintf1("system sem realease n is negative!\n");
        return false;
    }
    return modifySemaphore(n);
}
