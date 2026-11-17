// Combo index header for dodsp_suncascadedebug_ps30.fxc. Hand-written in Valve's .inc format:
// keep it in step with the .fxc's combo lines:  // STATIC: "FILTERMODE" "0..2"   (no dynamic)
#include "shaderlib/cshader.h"
class dodsp_suncascadedebug_ps30_Static_Index
{
private:
	int m_nFILTERMODE;
#ifdef _DEBUG
	bool m_bFILTERMODE;
#endif
public:
	void SetFILTERMODE( int i )
	{
		Assert( i >= 0 && i <= 2 );
		m_nFILTERMODE = i;
#ifdef _DEBUG
		m_bFILTERMODE = true;
#endif
	}
	void SetFILTERMODE( bool i )
	{
		m_nFILTERMODE = i ? 1 : 0;
#ifdef _DEBUG
		m_bFILTERMODE = true;
#endif
	}
	dodsp_suncascadedebug_ps30_Static_Index()
	{
#ifdef _DEBUG
		m_bFILTERMODE = false;
#endif
		m_nFILTERMODE = 0;
	}
	int GetIndex()
	{
#ifdef _DEBUG
		Assert( m_bFILTERMODE );
#endif
		return ( 1 * m_nFILTERMODE ) + 0;
	}
};
#define shaderStaticTest_dodsp_suncascadedebug_ps30 psh_forgot_to_set_static_FILTERMODE + 0
class dodsp_suncascadedebug_ps30_Dynamic_Index
{
public:
	dodsp_suncascadedebug_ps30_Dynamic_Index() {}
	int GetIndex() { return 0; }
};
#define shaderDynamicTest_dodsp_suncascadedebug_ps30 0
