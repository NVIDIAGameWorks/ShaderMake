#pragma once

struct IUnknown;

template<typename T> struct ComPtr
{
    ComPtr(T* lComPtr = nullptr) : m_ComPtr(lComPtr)
    {
        static_assert(std::is_base_of<IUnknown, T>::value, "T needs to be IUnknown based");

        if (m_ComPtr)
            m_ComPtr->AddRef();
    }

    ComPtr(const ComPtr<T>& lComPtrObj)
    {
        static_assert(std::is_base_of<IUnknown, T>::value, "T needs to be IUnknown based");

        m_ComPtr = lComPtrObj.m_ComPtr;

        if (m_ComPtr)
            m_ComPtr->AddRef();
    }

    ComPtr(ComPtr<T>&& lComPtrObj)
    {
        m_ComPtr = lComPtrObj.m_ComPtr;
        lComPtrObj.m_ComPtr = nullptr;
    }

    T* operator=(T* lComPtr)
    {
        if (m_ComPtr)
            m_ComPtr->Release();

        m_ComPtr = lComPtr;

        if (m_ComPtr)
            m_ComPtr->AddRef();

        return m_ComPtr;
    }

    T* operator=(const ComPtr<T>& lComPtrObj)
    {
        if (m_ComPtr)
            m_ComPtr->Release();

        m_ComPtr = lComPtrObj.m_ComPtr;

        if (m_ComPtr)
            m_ComPtr->AddRef();

        return m_ComPtr;
    }

    ~ComPtr()
    {
        if (m_ComPtr)
        {
            m_ComPtr->Release();
            m_ComPtr = nullptr;
        }
    }

    operator T*() const
    { return m_ComPtr; }

    T* GetInterface() const
    { return m_ComPtr; }

    T& operator*() const
    { return *m_ComPtr; }

    T** operator&()
    { return &m_ComPtr; }

    T* operator->() const
    { return m_ComPtr; }

    bool operator!() const
    { return (nullptr == m_ComPtr); }

    bool operator<(T* lComPtr) const
    {
        return m_ComPtr < lComPtr;
    }

    bool operator!=(T* lComPtr) const
    { return !operator==(lComPtr); }

    bool operator==(T* lComPtr) const
    { return m_ComPtr == lComPtr; }

protected:
    T* m_ComPtr;
};
