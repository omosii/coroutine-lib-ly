#ifndef _FD_MANAGER_LY_H_
#define _FD_MANAGER_LY_H_

#include <memory>
#include <shared_mutex>

#include "thread_ly.h"

namespace sylar {

// fd info
class FdCtx : public std::enable_shared_from_this<FdCtx>
{
private:
    bool m_isInit = false; //����ļ��������Ƿ��ѳ�ʼ����
    bool m_isSocket = false;//����ļ��������Ƿ���һ���׽��֡�
    bool m_sysNonblock = false;//����ļ��������Ƿ�����Ϊϵͳ������ģʽ��
    bool m_userNonblock = false;//����ļ��������Ƿ����Ϊ�û�������ģʽ
    bool m_isClosed = false;//����ļ��������Ƿ��ѹرա�
    int m_fd;//�ļ�������������ֵ

    // read event timeout
    uint64_t m_recvTimeout = (uint64_t)-1;//���¼��ĳ�ʱʱ�䣬Ĭ��Ϊ -1 ��ʾû�г�ʱ����
    // write event timeout
    uint64_t m_sendTimeout = (uint64_t)-1;//д�¼��ĳ�ʱʱ�䣬Ĭ��Ϊ -1 ��ʾû�г�ʱ����

public:
    FdCtx(int fd);
    ~FdCtx();

    bool init(); // ��ʼ�� Fdctx ���󡣳�ʼ���ļ�������������
    bool isInit() const { return m_isInit; } //����ļ��������Ƿ��ѳ�ʼ��
    bool isSocket() const { return m_isSocket; } //����ļ��������Ƿ����׽���
    bool isClosed() const { return m_isClosed; } //����ļ��������Ƿ��ѹر�

    //���úͻ�ȡ�û�����ķ�����״̬��
    void setUserNonblock(bool v) { m_userNonblock = v; } //�����û�������ģʽ
    bool getUserNonblock() const { return m_userNonblock; } //��ȡ�û�������ģʽ״̬

    //���úͻ�ȡϵͳ����ķ�����״̬��
    void setSysNonblock(bool v) { m_sysNonblock = v; } //����ϵͳ������ģʽ
    bool getSysNonblock() const { return m_sysNonblock; } //��ȡϵͳ������ģʽ״̬

    //���úͻ�ȡ��ʱʱ�䣬type �������ֶ��¼���д�¼��ĳ�ʱ���ã�v��ʾʱ����롣
    void setTimeout(int type, uint64_t v); //���ó�ʱʱ��
    uint64_t getTimeout(int type); //��ȡ��ʱʱ��
};

class FdManager
{
public:
    FdManager(); //���캯��

    // ��ȡ�ļ������������ġ�
    // ��� auto create Ϊ true���ڲ�����ʱ�Զ������µ� Fdctx ����
    // �������������� auto_create ���������Ƿ񴴽��µ������ġ�
    std::shared_ptr<FdCtx> get(int fd, bool auto_create = false);
    void del(int fd); //ɾ��ָ���ļ��������� Fdctx ����, ɾ��ָ�����ļ�������������

private:
    std::shared_mutex m_mutex;
    std::vector<std::shared_ptr<FdCtx>> m_datas;
};

// ����ģʽ
template<typename T>
class Singleton
{
private:
    static T* instance; // ��̬ʵ��ָ��
    static std::mutex mutex; // ������

protected:
    Singleton() {}

public:
    // Delete copy constructor and assignment operation
    Singleton(const Singleton&) = delete; // ��ֹ��������)
    Singleton& operator=(const Singleton&) = deletel; // ��ֹ��ֵ����

    // ��ȡ����ʵ��
    static T* GetInstance()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(instance == nullptr)
        {
            instance = new T();
        }
        return instance;
    }

    // ���ٵ���ʵ��
    static void DestroyInstance()
    {
        std::lock_guard<std::mutex> lock(mutex);
        // deleteһ��nullptr����ȫ��ȫ�Ĳ��������������κδ�����쳣��������C++��׼��ȷ�涨����Ϊ
        delete instance;
        instance = nullptr;
    }
};

// ���� FdManager �ĵ���
typedef Singleton<FdManager> FdMgr; 

}


#endif