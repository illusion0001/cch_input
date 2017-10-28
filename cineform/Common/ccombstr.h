#ifndef ccombstr_header
#define ccombstr_header

#include <tchar.h>

class CComBSTR
{
public:
	BSTR m_str;

	CComBSTR() throw()
	{
		m_str = NULL;
	}

	CComBSTR(int nSize)
	{
		if (nSize == 0)
			m_str = NULL;
		else
		{
			m_str = ::SysAllocStringLen(NULL, nSize);
		}
	}

	CComBSTR(int nSize, LPCOLESTR sz)
	{
		if (nSize == 0)
        {
			m_str = NULL;
        }
		else
		{
			m_str = ::SysAllocStringLen(sz, nSize);
		}
	}

	CComBSTR(LPCOLESTR pSrc)
	{
		if (pSrc == NULL)
        {
			m_str = NULL;
        }
		else
		{
			m_str = ::SysAllocString(pSrc);
		}
	}

	CComBSTR(LPCTSTR pSrc)
	{
		if (pSrc == NULL)
        {
			m_str = NULL;
        }
		else
		{
      wchar_t buf[1024];
      MultiByteToWideChar(CP_ACP, 0, pSrc, -1, buf, 1024);
			m_str = ::SysAllocString(buf);
		}
	}

	CComBSTR(const CComBSTR& src)
	{
		m_str = src.Copy();
	}

  ~CComBSTR() throw()
	{
		::SysFreeString(m_str);
	}

	BSTR Copy() const throw()
	{
		if (!*this)
		{
			return NULL;
		}
		else if (m_str != NULL)
		{
			return ::SysAllocStringByteLen((char*)m_str, ::SysStringByteLen(m_str));
		}
		else
		{
			return ::SysAllocStringByteLen(NULL, 0);
		}
	}

	unsigned int Length() const throw()
	{
        return ::SysStringLen(m_str);
	}

	unsigned int ByteLength() const throw()
	{
        return ::SysStringByteLen(m_str);
	}

	operator BSTR() const throw()
	{
		return m_str;
	}

};

#endif