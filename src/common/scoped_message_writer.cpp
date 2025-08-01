#include "scoped_message_writer.h"

#define ANTD_INTEGRATION_TEST_HOOKS_IMPLEMENTATION
#include "common/antd_integration_test_hooks.h"

// NOTE(antd): This file only exists because I need a way to hook into the
// message writer for integration tests. Originally this was a header only file,
// which means it needs to know the implementation of
// antd_integration_test_hooks.h functions which isn't possible to expose in
// just the header because of the One Definition Rule.
//   - doyle 2018-11-08

tools::scoped_message_writer::~scoped_message_writer()
{
  if (m_flush)
  {
    m_flush = false;

#if defined(ANTD_ENABLE_INTEGRATION_TEST_HOOKS)
    std::cout << m_oss.str();
    antd::write_redirected_stdout_to_shared_mem();
    return;
#endif

    MCLOG_FILE(m_log_level, "msgwriter", m_oss.str());
    if (epee::console_color_default == m_color)
    {
      std::cout << m_oss.str();
    }
    else
    {
      PAUSE_READLINE();
      set_console_color(m_color, m_bright);
      std::cout << m_oss.str();
      epee::reset_console_color();
    }
    std::cout << std::endl;
  }
}
