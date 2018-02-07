/*
 * Copyright (c) 2016 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <netinet/tcp.h>

#include <vcl/vcom_socket_wrapper.h>
#include <vcl/vcom.h>
#include <sys/time.h>

#include <vcl/vppcom.h>
#include <vppinfra/time.h>
#include <vppinfra/bitmap.h>

#define HAVE_CONSTRUCTOR_ATTRIBUTE
#ifdef HAVE_CONSTRUCTOR_ATTRIBUTE
#define CONSTRUCTOR_ATTRIBUTE                       \
    __attribute__ ((constructor))
#else
#define CONSTRUCTOR_ATTRIBUTE
#endif /* HAVE_CONSTRUCTOR_ATTRIBUTE */

#define HAVE_DESTRUCTOR_ATTRIBUTE
#ifdef HAVE_DESTRUCTOR_ATTRIBUTE
#define DESTRUCTOR_ATTRIBUTE                        \
    __attribute__ ((destructor))
#else
#define DESTRUCTOR_ATTRIBUTE
#endif

typedef struct
{
  int init;
  char app_name[VCOM_APP_NAME_MAX];
  u32 sid_bit_val;
  u32 sid_bit_mask;
  u32 debug;
  u8 *io_buffer;
  clib_time_t clib_time;
  clib_bitmap_t *rd_bitmap;
  clib_bitmap_t *wr_bitmap;
  clib_bitmap_t *ex_bitmap;
  clib_bitmap_t *sid_rd_bitmap;
  clib_bitmap_t *sid_wr_bitmap;
  clib_bitmap_t *sid_ex_bitmap;
  clib_bitmap_t *libc_rd_bitmap;
  clib_bitmap_t *libc_wr_bitmap;
  clib_bitmap_t *libc_ex_bitmap;
  vcl_poll_t *vcl_poll;
  u8 select_vcl;
  u8 epoll_wait_vcl;
} vcom_main_t;
#define VCOM_DEBUG vcom->debug

static vcom_main_t vcom_main = {
  .sid_bit_val = (1 << VCOM_SID_BIT_MIN),
  .sid_bit_mask = (1 << VCOM_SID_BIT_MIN) - 1,
  .debug = VCOM_DEBUG_INIT,
};

static vcom_main_t *vcom = &vcom_main;

/*
 * RETURN:  0 on success or -1 on error.
 * */
static inline void
vcom_set_app_name (char *app_name)
{
  int rv = snprintf (vcom->app_name, VCOM_APP_NAME_MAX,
		     "vcom-%d-%s", getpid (), app_name);

  if (rv >= VCOM_APP_NAME_MAX)
    app_name[VCOM_APP_NAME_MAX - 1] = 0;
}

static inline char *
vcom_get_app_name ()
{
  if (vcom->app_name[0] == '\0')
    vcom_set_app_name ("app");

  return vcom->app_name;
}

static inline int
vcom_fd_from_sid (u32 sid)
{
  if (PREDICT_FALSE (sid >= vcom->sid_bit_val))
    return -EMFILE;
  else
    return (sid | vcom->sid_bit_val);
}

static inline int
vcom_fd_is_sid (int fd)
{
  return ((u32) fd & vcom->sid_bit_val) ? 1 : 0;
}

static inline u32
vcom_sid_from_fd (int fd)
{
  return (vcom_fd_is_sid (fd) ? ((u32) fd & vcom->sid_bit_mask) :
	  INVALID_SESSION_ID);
}

static inline int
vcom_init (void)
{
  int rv = 0;

  if (PREDICT_FALSE (!vcom->init))
    {
      vcom->init = 1;
      rv = vppcom_app_create (vcom_get_app_name ());
      if (rv == VPPCOM_OK)
	{
	  char *env_var_str = getenv (VCOM_ENV_DEBUG);
	  if (env_var_str)
	    {
	      u32 tmp;
	      if (sscanf (env_var_str, "%u", &tmp) != 1)
		clib_warning ("LDP<%d>: WARNING: Invalid VCOM debug level "
			      "specified in the env var " VCOM_ENV_DEBUG
			      " (%s)!", getpid (), env_var_str);
	      else
		{
		  vcom->debug = tmp;
		  clib_warning ("LDP<%d>: configured VCOM debug level (%u) "
				"from the env var " VCOM_ENV_DEBUG "!",
				getpid (), vcom->debug);
		}
	    }

	  env_var_str = getenv (VCOM_ENV_APP_NAME);
	  if (env_var_str)
	    {
	      vcom_set_app_name (env_var_str);
	      clib_warning ("LDP<%d>: configured VCOM app name (%s) "
			    "from the env var " VCOM_ENV_APP_NAME "!",
			    getpid (), vcom->app_name);
	    }

	  env_var_str = getenv (VCOM_ENV_SID_BIT);
	  if (env_var_str)
	    {
	      u32 sb;
	      if (sscanf (env_var_str, "%u", &sb) != 1)
		{
		  clib_warning ("LDP<%d>: WARNING: Invalid VCOM sid bit "
				"specified in the env var "
				VCOM_ENV_SID_BIT " (%s)!"
				"sid bit value %d (0x%x)",
				getpid (), env_var_str,
				vcom->sid_bit_val, vcom->sid_bit_val);
		}
	      else if (sb < VCOM_SID_BIT_MIN)
		{
		  vcom->sid_bit_val = (1 << VCOM_SID_BIT_MIN);
		  vcom->sid_bit_mask = vcom->sid_bit_val - 1;

		  clib_warning ("LDP<%d>: WARNING: VCOM sid bit (%u) "
				"specified in the env var "
				VCOM_ENV_SID_BIT " (%s) is too small. "
				"Using VCOM_SID_BIT_MIN (%d)! "
				"sid bit value %d (0x%x)",
				getpid (), sb, env_var_str, VCOM_SID_BIT_MIN,
				vcom->sid_bit_val, vcom->sid_bit_val);
		}
	      else if (sb > VCOM_SID_BIT_MAX)
		{
		  vcom->sid_bit_val = (1 << VCOM_SID_BIT_MAX);
		  vcom->sid_bit_mask = vcom->sid_bit_val - 1;

		  clib_warning ("LDP<%d>: WARNING: VCOM sid bit (%u) "
				"specified in the env var "
				VCOM_ENV_SID_BIT " (%s) is too big. "
				"Using VCOM_SID_BIT_MAX (%d)! "
				"sid bit value %d (0x%x)",
				getpid (), sb, env_var_str, VCOM_SID_BIT_MAX,
				vcom->sid_bit_val, vcom->sid_bit_val);
		}
	      else
		{
		  vcom->sid_bit_val = (1 << sb);
		  vcom->sid_bit_mask = vcom->sid_bit_val - 1;

		  clib_warning ("LDP<%d>: configured VCOM sid bit (%u) "
				"from " VCOM_ENV_SID_BIT
				"!  sid bit value %d (0x%x)", getpid (),
				sb, vcom->sid_bit_val, vcom->sid_bit_val);
		}
	    }

	  clib_time_init (&vcom->clib_time);
	  clib_warning ("LDP<%d>: VCOM initialization: done!", getpid ());
	}
      else
	{
	  fprintf (stderr, "\nLDP<%d>: ERROR: vcom_init: vppcom_app_create()"
		   " failed!  rv = %d (%s)\n",
		   getpid (), rv, vppcom_retval_str (rv));
	  vcom->init = 0;
	}
    }
  return rv;
}

int
close (int fd)
{
  int rv;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      int epfd;

      func_str = "vppcom_session_attr[GET_LIBC_EPFD]";
      epfd = vppcom_session_attr (sid, VPPCOM_ATTR_GET_LIBC_EPFD, 0, 0);
      if (epfd > 0)
	{
	  func_str = "libc_close";

	  if (VCOM_DEBUG > 0)
	    clib_warning
	      ("LDP<%d>: fd %d (0x%x): calling %s(): epfd %u (0x%x)",
	       getpid (), fd, fd, func_str, epfd, epfd);

	  rv = libc_close (epfd);
	  if (rv < 0)
	    {
	      u32 size = sizeof (epfd);
	      epfd = 0;

	      (void) vppcom_session_attr (sid, VPPCOM_ATTR_SET_LIBC_EPFD,
					  &epfd, &size);
	    }
	}
      else if (PREDICT_FALSE (epfd < 0))
	{
	  errno = -epfd;
	  rv = -1;
	  goto done;
	}

      func_str = "vppcom_session_close";

      if (VCOM_DEBUG > 0)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): sid %u (0x%x)",
		      getpid (), fd, fd, func_str, sid, sid);

      rv = vppcom_session_close (sid);
      if (rv != VPPCOM_OK)
	{
	  errno = -rv;
	  rv = -1;
	}
    }
  else
    {
      func_str = "libc_close";

      if (VCOM_DEBUG > 0)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s()",
		      getpid (), fd, fd, func_str);

      rv = libc_close (fd);
    }

done:
  if (VCOM_DEBUG > 0)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, rv, rv);
    }
  return rv;
}

ssize_t
read (int fd, void *buf, size_t nbytes)
{
  ssize_t size;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      func_str = "vppcom_session_read";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "sid %u (0x%x), buf %p, nbytes %u", getpid (),
		      fd, fd, func_str, sid, sid, buf, nbytes);

      size = vppcom_session_read (sid, buf, nbytes);
      if (size < 0)
	{
	  errno = -size;
	  size = -1;
	}
    }
  else
    {
      func_str = "libc_read";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "buf %p, nbytes %u", getpid (),
		      fd, fd, func_str, buf, nbytes);

      size = libc_read (fd, buf, nbytes);
    }

  if (VCOM_DEBUG > 2)
    {
      if (size < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, size, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, size, size);
    }
  return size;
}

ssize_t
readv (int fd, const struct iovec * iov, int iovcnt)
{
  const char *func_str;
  ssize_t size = 0;
  u32 sid = vcom_sid_from_fd (fd);
  int rv = 0, i, total = 0;

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      func_str = "vppcom_session_read";
      do
	{
	  for (i = 0; i < iovcnt; ++i)
	    {
	      if (VCOM_DEBUG > 2)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s() [%d]: "
			      "sid %u (0x%x), iov %p, iovcnt %d, total %d",
			      getpid (), fd, fd, func_str, i, sid, sid,
			      iov, iovcnt, total);

	      rv = vppcom_session_read (sid, iov[i].iov_base, iov[i].iov_len);
	      if (rv < 0)
		break;
	      else
		{
		  total += rv;
		  if (rv < iov[i].iov_len)
		    {
		      if (VCOM_DEBUG > 2)
			clib_warning ("LDP<%d>: fd %d (0x%x): "
				      "rv (%d) < iov[%d].iov_len (%d)",
				      getpid (), fd, fd, rv, i,
				      iov[i].iov_len);
		      break;
		    }
		}
	    }
	}
      while ((rv >= 0) && (total == 0));

      if (rv < 0)
	{
	  errno = -rv;
	  size = -1;
	}
      else
	size = total;
    }
  else
    {
      func_str = "libc_readv";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "iov %p, iovcnt %d", getpid (), fd, fd, iov, iovcnt);

      size = libc_readv (fd, iov, iovcnt);
    }

  if (VCOM_DEBUG > 2)
    {
      if (size < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, size, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, size, size);
    }
  return size;
}

ssize_t
write (int fd, const void *buf, size_t nbytes)
{
  const char *func_str;
  ssize_t size = 0;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      func_str = "vppcom_session_write";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "sid %u (0x%x), buf %p, nbytes %u", getpid (),
		      fd, fd, func_str, sid, sid, buf, nbytes);

      size = vppcom_session_write (sid, (void *) buf, nbytes);
      if (size < 0)
	{
	  errno = -size;
	  size = -1;
	}
    }
  else
    {
      func_str = "libc_write";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "buf %p, nbytes %u", getpid (),
		      fd, fd, func_str, buf, nbytes);

      size = libc_write (fd, buf, nbytes);
    }

  if (VCOM_DEBUG > 2)
    {
      if (size < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, size, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, size, size);
    }
  return size;
}

ssize_t
writev (int fd, const struct iovec * iov, int iovcnt)
{
  const char *func_str;
  ssize_t size = 0, total = 0;
  u32 sid = vcom_sid_from_fd (fd);
  int i, rv = 0;

  /*
   * Use [f]printf() instead of clib_warning() to prevent recursion SIGSEGV.
   */

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      func_str = "vppcom_session_write";
      do
	{
	  for (i = 0; i < iovcnt; ++i)
	    {
	      if (VCOM_DEBUG > 4)
		printf ("%s:%d: LDP<%d>: fd %d (0x%x): calling %s() [%d]: "
			"sid %u (0x%x), buf %p, nbytes %ld, total %ld",
			__func__, __LINE__, getpid (), fd, fd, func_str,
			i, sid, sid, iov[i].iov_base, iov[i].iov_len, total);

	      rv = vppcom_session_write (sid, iov[i].iov_base,
					 iov[i].iov_len);
	      if (rv < 0)
		break;
	      else
		{
		  total += rv;
		  if (rv < iov[i].iov_len)
		    {
		      if (VCOM_DEBUG > 4)
			printf ("%s:%d: LDP<%d>: fd %d (0x%x): "
				"rv (%d) < iov[%d].iov_len (%ld)",
				__func__, __LINE__, getpid (), fd, fd,
				rv, i, iov[i].iov_len);
		      break;
		    }
		}
	    }
	}
      while ((rv >= 0) && (total == 0));

      if (rv < 0)
	{
	  errno = -rv;
	  size = -1;
	}
      else
	size = total;
    }
  else
    {
      func_str = "libc_writev";

      if (VCOM_DEBUG > 4)
	printf ("%s:%d: LDP<%d>: fd %d (0x%x): calling %s(): "
		"iov %p, iovcnt %d\n", __func__, __LINE__, getpid (),
		fd, fd, func_str, iov, iovcnt);

      size = libc_writev (fd, iov, iovcnt);
    }

  if (VCOM_DEBUG > 4)
    {
      if (size < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  fprintf (stderr,
		   "%s:%d: LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
		   "rv %ld, errno = %d\n", __func__, __LINE__, getpid (), fd,
		   fd, func_str, size, errno_val);
	  errno = errno_val;
	}
      else
	printf ("%s:%d: LDP<%d>: fd %d (0x%x): returning %ld\n",
		__func__, __LINE__, getpid (), fd, fd, size);
    }
  return size;
}

int
fcntl (int fd, int cmd, ...)
{
  const char *func_str = __func__;
  int rv = 0;
  va_list ap;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  va_start (ap, cmd);
  if (sid != INVALID_SESSION_ID)
    {
      int flags = va_arg (ap, int);
      u32 size;

      size = sizeof (flags);
      rv = -EOPNOTSUPP;
      switch (cmd)
	{
	case F_SETFL:
	  func_str = "vppcom_session_attr[SET_FLAGS]";
	  if (VCOM_DEBUG > 2)
	    clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			  "sid %u (0x%x) flags %d (0x%x), size %d",
			  getpid (), fd, fd, func_str, sid, sid,
			  flags, flags, size);

	  rv =
	    vppcom_session_attr (sid, VPPCOM_ATTR_SET_FLAGS, &flags, &size);
	  break;

	case F_GETFL:
	  func_str = "vppcom_session_attr[GET_FLAGS]";
	  if (VCOM_DEBUG > 2)
	    clib_warning
	      ("LDP<%d>: fd %d (0x%x): calling %s(): sid %u (0x%x), "
	       "flags %d (0x%x), size %d", getpid (), fd, fd, func_str, sid,
	       sid, flags, flags, size);

	  rv =
	    vppcom_session_attr (sid, VPPCOM_ATTR_GET_FLAGS, &flags, &size);
	  if (rv == VPPCOM_OK)
	    {
	      if (VCOM_DEBUG > 2)
		clib_warning ("LDP<%d>: fd %d (0x%x), cmd %d (F_GETFL): "
			      "%s() returned flags %d (0x%x)",
			      getpid (), fd, fd, cmd, func_str, flags, flags);
	      rv = flags;
	    }
	  break;

	default:
	  rv = -EOPNOTSUPP;
	  break;
	}
      if (rv < 0)
	{
	  errno = -rv;
	  rv = -1;
	}
    }
  else
    {
      func_str = "libc_vfcntl";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): cmd %d",
		      getpid (), fd, fd, func_str, cmd);

      rv = libc_vfcntl (fd, cmd, ap);
    }

  va_end (ap);

  if (VCOM_DEBUG > 2)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, rv, rv);
    }
  return rv;
}

int
ioctl (int fd, unsigned long int cmd, ...)
{
  const char *func_str;
  int rv;
  va_list ap;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  va_start (ap, cmd);
  if (sid != INVALID_SESSION_ID)
    {
      func_str = "vppcom_session_attr[GET_NREAD]";

      switch (cmd)
	{
	case FIONREAD:
	  if (VCOM_DEBUG > 2)
	    clib_warning
	      ("LDP<%d>: fd %d (0x%x): calling  %s(): sid %u (0x%x)",
	       getpid (), fd, fd, func_str, sid, sid);

	  rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_NREAD, 0, 0);
	  break;

	case FIONBIO:
	  {
	    u32 flags = va_arg (ap, int) ? O_NONBLOCK : 0;
	    u32 size = sizeof (flags);

	    /* TBD: When VPPCOM_ATTR_[GS]ET_FLAGS supports flags other than
	     *      non-blocking, the flags should be read here and merged
	     *      with O_NONBLOCK.
	     */
	    if (VCOM_DEBUG > 2)
	      clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			    "sid %u (0x%x), flags %d (0x%x), size %d",
			    getpid (), fd, fd, func_str, sid, sid,
			    flags, flags, size);

	    rv = vppcom_session_attr (sid, VPPCOM_ATTR_SET_FLAGS, &flags,
				      &size);
	  }
	  break;

	default:
	  rv = -EOPNOTSUPP;
	  break;
	}
      if (rv < 0)
	{
	  errno = -rv;
	  rv = -1;
	}
    }
  else
    {
      func_str = "libc_vioctl";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): cmd %d",
		      getpid (), fd, fd, func_str, cmd);

      rv = libc_vioctl (fd, cmd, ap);
    }

  if (VCOM_DEBUG > 2)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, rv, rv);
    }
  va_end (ap);
  return rv;
}

int
vcom_pselect (int nfds, fd_set * __restrict readfds,
	      fd_set * __restrict writefds,
	      fd_set * __restrict exceptfds,
	      const struct timespec *__restrict timeout,
	      const __sigset_t * __restrict sigmask)
{
  int rv;
  char *func_str = "##";
  f64 time_out;
  int fd;
  uword sid_bits, sid_bits_set, libc_bits, libc_bits_set;
  u32 minbits = clib_max (nfds, BITS (uword));
  u32 sid;

  if (nfds < 0)
    {
      errno = EINVAL;
      return -1;
    }

  if (nfds <= vcom->sid_bit_val)
    {
      func_str = "libc_pselect";

      if (VCOM_DEBUG > 3)
	clib_warning
	  ("LDP<%d>: calling %s(): nfds %d, readfds %p, writefds %p, "
	   "exceptfds %p, timeout %p, sigmask %p", getpid (), func_str, nfds,
	   readfds, writefds, exceptfds, timeout, sigmask);

      rv = libc_pselect (nfds, readfds, writefds, exceptfds,
			 timeout, sigmask);
      goto done;
    }

  if (PREDICT_FALSE (vcom->sid_bit_val > FD_SETSIZE / 2))
    {
      clib_warning ("LDP<%d>: ERROR: VCOM sid bit value %d (0x%x) > "
		    "FD_SETSIZE/2 %d (0x%x)!", getpid (),
		    vcom->sid_bit_val, vcom->sid_bit_val,
		    FD_SETSIZE / 2, FD_SETSIZE / 2);
      errno = EOVERFLOW;
      return -1;
    }

  if (timeout)
    {
      time_out = (timeout->tv_sec == 0 && timeout->tv_nsec == 0) ?
	(f64) 0 : (f64) timeout->tv_sec +
	(f64) timeout->tv_nsec / (f64) 1000000000 +
	(f64) (timeout->tv_nsec % 1000000000) / (f64) 1000000000;

      /* select as fine grained sleep */
      if (!nfds)
	{
	  if (VCOM_DEBUG > 3)
	    clib_warning ("LDP<%d>: sleeping for %f seconds",
			  getpid (), time_out);

	  time_out += clib_time_now (&vcom->clib_time);
	  while (clib_time_now (&vcom->clib_time) < time_out)
	    ;
	  return 0;
	}
    }
  else if (!nfds)
    {
      errno = EINVAL;
      return -1;
    }
  else
    time_out = -1;

  sid_bits = libc_bits = 0;
  if (readfds)
    {
      clib_bitmap_validate (vcom->sid_rd_bitmap, minbits);
      clib_bitmap_validate (vcom->libc_rd_bitmap, minbits);
      clib_bitmap_validate (vcom->rd_bitmap, minbits);
      clib_memcpy (vcom->rd_bitmap, readfds,
		   vec_len (vcom->rd_bitmap) * sizeof (clib_bitmap_t));
      FD_ZERO (readfds);

      /* *INDENT-OFF* */
      clib_bitmap_foreach (fd, vcom->rd_bitmap,
        ({
          sid = vcom_sid_from_fd (fd);
          if (VCOM_DEBUG > 3)
            clib_warning ("LDP<%d>: readfds: fd %d (0x%x), sid %u (0x%x)",
                          getpid (), fd, fd, sid, sid);
          if (sid == INVALID_SESSION_ID)
            clib_bitmap_set_no_check (vcom->libc_rd_bitmap, fd, 1);
          else
            clib_bitmap_set_no_check (vcom->sid_rd_bitmap, sid, 1);
        }));
      /* *INDENT-ON* */

      sid_bits_set = clib_bitmap_last_set (vcom->sid_rd_bitmap) + 1;
      sid_bits = (sid_bits_set > sid_bits) ? sid_bits_set : sid_bits;

      libc_bits_set = clib_bitmap_last_set (vcom->libc_rd_bitmap) + 1;
      libc_bits = (libc_bits_set > libc_bits) ? libc_bits_set : libc_bits;

      if (VCOM_DEBUG > 3)
	clib_warning ("LDP<%d>: readfds: sid_bits_set %d, sid_bits %d, "
		      "libc_bits_set %d, libc_bits %d", getpid (),
		      sid_bits_set, sid_bits, libc_bits_set, libc_bits);
    }
  if (writefds)
    {
      clib_bitmap_validate (vcom->sid_wr_bitmap, minbits);
      clib_bitmap_validate (vcom->libc_wr_bitmap, minbits);
      clib_bitmap_validate (vcom->wr_bitmap, minbits);
      clib_memcpy (vcom->wr_bitmap, writefds,
		   vec_len (vcom->wr_bitmap) * sizeof (clib_bitmap_t));
      FD_ZERO (writefds);

      /* *INDENT-OFF* */
      clib_bitmap_foreach (fd, vcom->wr_bitmap,
        ({
          sid = vcom_sid_from_fd (fd);
          if (VCOM_DEBUG > 3)
            clib_warning ("LDP<%d>: writefds: fd %d (0x%x), sid %u (0x%x)",
                          getpid (), fd, fd, sid, sid);
          if (sid == INVALID_SESSION_ID)
            clib_bitmap_set_no_check (vcom->libc_wr_bitmap, fd, 1);
          else
            clib_bitmap_set_no_check (vcom->sid_wr_bitmap, sid, 1);
        }));
      /* *INDENT-ON* */

      sid_bits_set = clib_bitmap_last_set (vcom->sid_wr_bitmap) + 1;
      sid_bits = (sid_bits_set > sid_bits) ? sid_bits_set : sid_bits;

      libc_bits_set = clib_bitmap_last_set (vcom->libc_wr_bitmap) + 1;
      libc_bits = (libc_bits_set > libc_bits) ? libc_bits_set : libc_bits;

      if (VCOM_DEBUG > 3)
	clib_warning ("LDP<%d>: writefds: sid_bits_set %d, sid_bits %d, "
		      "libc_bits_set %d, libc_bits %d", getpid (),
		      sid_bits_set, sid_bits, libc_bits_set, libc_bits);
    }
  if (exceptfds)
    {
      clib_bitmap_validate (vcom->sid_ex_bitmap, minbits);
      clib_bitmap_validate (vcom->libc_ex_bitmap, minbits);
      clib_bitmap_validate (vcom->ex_bitmap, minbits);
      clib_memcpy (vcom->ex_bitmap, exceptfds,
		   vec_len (vcom->ex_bitmap) * sizeof (clib_bitmap_t));
      FD_ZERO (exceptfds);

      /* *INDENT-OFF* */
      clib_bitmap_foreach (fd, vcom->ex_bitmap,
        ({
          sid = vcom_sid_from_fd (fd);
          if (VCOM_DEBUG > 3)
            clib_warning ("LDP<%d>: exceptfds: fd %d (0x%x), sid %u (0x%x)",
                          getpid (), fd, fd, sid, sid);
          if (sid == INVALID_SESSION_ID)
            clib_bitmap_set_no_check (vcom->libc_ex_bitmap, fd, 1);
          else
            clib_bitmap_set_no_check (vcom->sid_ex_bitmap, sid, 1);
        }));
      /* *INDENT-ON* */

      sid_bits_set = clib_bitmap_last_set (vcom->sid_ex_bitmap) + 1;
      sid_bits = (sid_bits_set > sid_bits) ? sid_bits_set : sid_bits;

      libc_bits_set = clib_bitmap_last_set (vcom->libc_ex_bitmap) + 1;
      libc_bits = (libc_bits_set > libc_bits) ? libc_bits_set : libc_bits;

      if (VCOM_DEBUG > 3)
	clib_warning ("LDP<%d>: exceptfds: sid_bits_set %d, sid_bits %d, "
		      "libc_bits_set %d, libc_bits %d", getpid (),
		      sid_bits_set, sid_bits, libc_bits_set, libc_bits);
    }

  if (PREDICT_FALSE (!sid_bits && !libc_bits))
    {
      errno = EINVAL;
      rv = -1;
      goto done;
    }

  do
    {
      if (sid_bits)
	{
	  if (!vcom->select_vcl)
	    {
	      func_str = "vppcom_select";

	      if (readfds)
		clib_memcpy (vcom->rd_bitmap, vcom->sid_rd_bitmap,
			     vec_len (vcom->rd_bitmap) *
			     sizeof (clib_bitmap_t));
	      if (writefds)
		clib_memcpy (vcom->wr_bitmap, vcom->sid_wr_bitmap,
			     vec_len (vcom->wr_bitmap) *
			     sizeof (clib_bitmap_t));
	      if (exceptfds)
		clib_memcpy (vcom->ex_bitmap, vcom->sid_ex_bitmap,
			     vec_len (vcom->ex_bitmap) *
			     sizeof (clib_bitmap_t));

	      rv = vppcom_select (sid_bits,
				  readfds ? vcom->rd_bitmap : NULL,
				  writefds ? vcom->wr_bitmap : NULL,
				  exceptfds ? vcom->ex_bitmap : NULL, 0);
	      if (rv < 0)
		{
		  errno = -rv;
		  rv = -1;
		}
	      else if (rv > 0)
		{
		  if (readfds)
		    {
                      /* *INDENT-OFF* */
                      clib_bitmap_foreach (sid, vcom->rd_bitmap,
                        ({
                          fd = vcom_fd_from_sid (sid);
                          if (PREDICT_FALSE (fd < 0))
                            {
                              errno = EBADFD;
                              rv = -1;
                              goto done;
                            }
                          FD_SET (fd, readfds);
                        }));
                      /* *INDENT-ON* */
		    }
		  if (writefds)
		    {
                      /* *INDENT-OFF* */
                      clib_bitmap_foreach (sid, vcom->wr_bitmap,
                        ({
                          fd = vcom_fd_from_sid (sid);
                          if (PREDICT_FALSE (fd < 0))
                            {
                              errno = EBADFD;
                              rv = -1;
                              goto done;
                            }
                          FD_SET (fd, writefds);
                        }));
                      /* *INDENT-ON* */
		    }
		  if (exceptfds)
		    {
                      /* *INDENT-OFF* */
                      clib_bitmap_foreach (sid, vcom->ex_bitmap,
                        ({
                          fd = vcom_fd_from_sid (sid);
                          if (PREDICT_FALSE (fd < 0))
                            {
                              errno = EBADFD;
                              rv = -1;
                              goto done;
                            }
                          FD_SET (fd, exceptfds);
                        }));
                      /* *INDENT-ON* */
		    }
		  vcom->select_vcl = 1;
		  goto done;
		}
	    }
	  else
	    vcom->select_vcl = 0;
	}
      if (libc_bits)
	{
	  struct timespec tspec;

	  func_str = "libc_pselect";

	  if (readfds)
	    clib_memcpy (readfds, vcom->libc_rd_bitmap,
			 vec_len (vcom->rd_bitmap) * sizeof (clib_bitmap_t));
	  if (writefds)
	    clib_memcpy (writefds, vcom->libc_wr_bitmap,
			 vec_len (vcom->wr_bitmap) * sizeof (clib_bitmap_t));
	  if (exceptfds)
	    clib_memcpy (exceptfds, vcom->libc_ex_bitmap,
			 vec_len (vcom->ex_bitmap) * sizeof (clib_bitmap_t));
	  tspec.tv_sec = tspec.tv_nsec = 0;
	  rv = libc_pselect (libc_bits,
			     readfds ? readfds : NULL,
			     writefds ? writefds : NULL,
			     exceptfds ? exceptfds : NULL, &tspec, sigmask);
	  if (rv != 0)
	    goto done;
	}
    }
  while ((time_out == -1) || (clib_time_now (&vcom->clib_time) < time_out));
  rv = 0;

done:
  /* TBD: set timeout to amount of time left */
  vec_reset_length (vcom->rd_bitmap);
  vec_reset_length (vcom->sid_rd_bitmap);
  vec_reset_length (vcom->libc_rd_bitmap);
  vec_reset_length (vcom->wr_bitmap);
  vec_reset_length (vcom->sid_wr_bitmap);
  vec_reset_length (vcom->libc_wr_bitmap);
  vec_reset_length (vcom->ex_bitmap);
  vec_reset_length (vcom->sid_ex_bitmap);
  vec_reset_length (vcom->libc_ex_bitmap);

  if (VCOM_DEBUG > 3)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: %s() failed! "
			"rv %d, errno = %d", getpid (),
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: returning %d (0x%x)", getpid (), rv, rv);
    }
  return rv;
}

int
select (int nfds, fd_set * __restrict readfds,
	fd_set * __restrict writefds,
	fd_set * __restrict exceptfds, struct timeval *__restrict timeout)
{
  struct timespec tspec;

  if (timeout)
    {
      tspec.tv_sec = timeout->tv_sec;
      tspec.tv_nsec = timeout->tv_usec * 1000;
    }
  return vcom_pselect (nfds, readfds, writefds, exceptfds,
		       timeout ? &tspec : NULL, NULL);
}

#ifdef __USE_XOPEN2K
int
pselect (int nfds, fd_set * __restrict readfds,
	 fd_set * __restrict writefds,
	 fd_set * __restrict exceptfds,
	 const struct timespec *__restrict timeout,
	 const __sigset_t * __restrict sigmask)
{
  return vcom_pselect (nfds, readfds, writefds, exceptfds, timeout, 0);
}
#endif

int
socket (int domain, int type, int protocol)
{
  const char *func_str;
  int rv;
  u8 is_nonblocking = type & SOCK_NONBLOCK ? 1 : 0;
  int sock_type = type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK);

  if ((errno = -vcom_init ()))
    return -1;

  if (((domain == AF_INET) || (domain == AF_INET6)) &&
      ((sock_type == SOCK_STREAM) || (sock_type == SOCK_DGRAM)))
    {
      int sid;
      u32 vrf = VPPCOM_VRF_DEFAULT;
      u8 proto = ((sock_type == SOCK_DGRAM) ?
		  VPPCOM_PROTO_UDP : VPPCOM_PROTO_TCP);

      func_str = "vppcom_session_create";

      if (VCOM_DEBUG > 0)
	clib_warning ("LDP<%d>: : calling %s(): vrf %u, "
		      "proto %u (%s), is_nonblocking %u",
		      getpid (), func_str, vrf, proto,
		      vppcom_proto_str (proto), is_nonblocking);

      sid = vppcom_session_create (vrf, proto, is_nonblocking);
      if (sid < 0)
	{
	  errno = -sid;
	  rv = -1;
	}
      else
	{
	  func_str = "vcom_fd_from_sid";
	  rv = vcom_fd_from_sid (sid);
	  if (rv < 0)
	    {
	      (void) vppcom_session_close (sid);
	      errno = -rv;
	      rv = -1;
	    }
	}
    }
  else
    {
      func_str = "libc_socket";

      if (VCOM_DEBUG > 0)
	clib_warning ("LDP<%d>: : calling %s()", getpid (), func_str);

      rv = libc_socket (domain, type, protocol);
    }

  if (VCOM_DEBUG > 0)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: %s() failed! "
			"rv %d, errno = %d",
			getpid (), func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: : returning fd %d (0x%x)", getpid (), rv, rv);
    }
  return rv;
}

/*
 * Create two new sockets, of type TYPE in domain DOMAIN and using
 * protocol PROTOCOL, which are connected to each other, and put file
 * descriptors for them in FDS[0] and FDS[1].  If PROTOCOL is zero,
 * one will be chosen automatically.
 * Returns 0 on success, -1 for errors.
 * */
int
socketpair (int domain, int type, int protocol, int fds[2])
{
  const char *func_str;
  int rv;
  int sock_type = type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK);

  if ((errno = -vcom_init ()))
    return -1;

  if (((domain == AF_INET) || (domain == AF_INET6)) &&
      ((sock_type == SOCK_STREAM) || (sock_type == SOCK_DGRAM)))
    {
      func_str = __func__;

      clib_warning ("LDP<%d>: LDP-TBD", getpid ());
      errno = ENOSYS;
      rv = -1;
    }
  else
    {
      func_str = "libc_socket";

      if (VCOM_DEBUG > 1)
	clib_warning ("LDP<%d>: : calling %s()", getpid (), func_str);

      rv = libc_socket (domain, type, protocol);
    }

  if (VCOM_DEBUG > 1)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: %s() failed! "
			"rv %d, errno = %d",
			getpid (), func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: : returning fd %d (0x%x)", getpid (), rv, rv);
    }
  return rv;
}

int
bind (int fd, __CONST_SOCKADDR_ARG addr, socklen_t len)
{
  int rv;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      vppcom_endpt_t ep;

      func_str = "vppcom_session_bind";

      ep.vrf = VPPCOM_VRF_DEFAULT;
      switch (addr->sa_family)
	{
	case AF_INET:
	  if (len != sizeof (struct sockaddr_in))
	    {
	      clib_warning
		("LDP<%d>: ERROR: fd %d (0x%x): sid %u (0x%x): Invalid "
		 "AF_INET addr len %u!", getpid (), fd, fd, sid, sid, len);
	      errno = EINVAL;
	      rv = -1;
	      goto done;
	    }
	  ep.is_ip4 = VPPCOM_IS_IP4;
	  ep.ip = (u8 *) & ((const struct sockaddr_in *) addr)->sin_addr;
	  ep.port = (u16) ((const struct sockaddr_in *) addr)->sin_port;
	  break;

	case AF_INET6:
	  if (len != sizeof (struct sockaddr_in6))
	    {
	      clib_warning
		("LDP<%d>: ERROR: fd %d (0x%x): sid %u (0x%x): Invalid "
		 "AF_INET6 addr len %u!", getpid (), fd, fd, sid, sid, len);
	      errno = EINVAL;
	      rv = -1;
	      goto done;
	    }
	  ep.is_ip4 = VPPCOM_IS_IP6;
	  ep.ip = (u8 *) & ((const struct sockaddr_in6 *) addr)->sin6_addr;
	  ep.port = (u16) ((const struct sockaddr_in6 *) addr)->sin6_port;
	  break;

	default:
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): sid %u (0x%x): "
			"Unsupported address family %u!",
			getpid (), fd, fd, sid, sid, addr->sa_family);
	  errno = EAFNOSUPPORT;
	  rv = -1;
	  goto done;
	}
      if (VCOM_DEBUG > 0)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): sid %u (0x%x), "
		      "addr %p, len %u",
		      getpid (), fd, fd, func_str, sid, sid, addr, len);

      rv = vppcom_session_bind (sid, &ep);
      if (rv != VPPCOM_OK)
	{
	  errno = -rv;
	  rv = -1;
	}
    }
  else
    {
      func_str = "libc_bind";

      if (VCOM_DEBUG > 0)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "addr %p, len %u",
		      getpid (), fd, fd, func_str, addr, len);

      rv = libc_bind (fd, addr, len);
    }

done:
  if (VCOM_DEBUG > 0)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, rv, rv);
    }
  return rv;
}

static inline int
vcom_copy_ep_to_sockaddr (__SOCKADDR_ARG addr, socklen_t * __restrict len,
			  vppcom_endpt_t * ep)
{
  int rv = 0;
  int sa_len, copy_len;

  if ((errno = -vcom_init ()))
    return -1;

  if (addr && len && ep)
    {
      addr->sa_family = (ep->is_ip4 == VPPCOM_IS_IP4) ? AF_INET : AF_INET6;
      switch (addr->sa_family)
	{
	case AF_INET:
	  ((struct sockaddr_in *) addr)->sin_port = ep->port;
	  if (*len > sizeof (struct sockaddr_in))
	    *len = sizeof (struct sockaddr_in);
	  sa_len = sizeof (struct sockaddr_in) - sizeof (struct in_addr);
	  copy_len = *len - sa_len;
	  if (copy_len > 0)
	    memcpy (&((struct sockaddr_in *) addr)->sin_addr, ep->ip,
		    copy_len);
	  break;

	case AF_INET6:
	  ((struct sockaddr_in6 *) addr)->sin6_port = ep->port;
	  if (*len > sizeof (struct sockaddr_in6))
	    *len = sizeof (struct sockaddr_in6);
	  sa_len = sizeof (struct sockaddr_in6) - sizeof (struct in6_addr);
	  copy_len = *len - sa_len;
	  if (copy_len > 0)
	    memcpy (((struct sockaddr_in6 *) addr)->sin6_addr.
		    __in6_u.__u6_addr8, ep->ip, copy_len);
	  break;

	default:
	  /* Not possible */
	  rv = -EAFNOSUPPORT;
	  break;
	}
    }
  return rv;
}

int
getsockname (int fd, __SOCKADDR_ARG addr, socklen_t * __restrict len)
{
  int rv;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      vppcom_endpt_t ep;
      u8 addr_buf[sizeof (struct in6_addr)];
      u32 size = sizeof (ep);

      ep.ip = addr_buf;
      func_str = "vppcom_session_attr[GET_LCL_ADDR]";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): sid %u (0x%x), "
		      "addr %p, len %u",
		      getpid (), fd, fd, func_str, sid, sid, addr, len);

      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_LCL_ADDR, &ep, &size);
      if (rv != VPPCOM_OK)
	{
	  errno = -rv;
	  rv = -1;
	}
      else
	{
	  rv = vcom_copy_ep_to_sockaddr (addr, len, &ep);
	  if (rv != VPPCOM_OK)
	    {
	      errno = -rv;
	      rv = -1;
	    }
	}
    }
  else
    {
      func_str = "libc_getsockname";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "addr %p, len %u",
		      getpid (), fd, fd, func_str, addr, len);

      rv = libc_getsockname (fd, addr, len);
    }

  if (VCOM_DEBUG > 2)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, rv, rv);
    }
  return rv;
}

int
connect (int fd, __CONST_SOCKADDR_ARG addr, socklen_t len)
{
  int rv;
  const char *func_str = __func__;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (!addr)
    {
      clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): NULL addr, len %u",
		    getpid (), fd, fd, len);
      errno = EINVAL;
      rv = -1;
      goto done;
    }

  if (sid != INVALID_SESSION_ID)
    {
      vppcom_endpt_t ep;

      func_str = "vppcom_session_connect";

      ep.vrf = VPPCOM_VRF_DEFAULT;
      switch (addr->sa_family)
	{
	case AF_INET:
	  if (len != sizeof (struct sockaddr_in))
	    {
	      clib_warning
		("LDP<%d>: fd %d (0x%x): ERROR sid %u (0x%x): Invalid "
		 "AF_INET addr len %u!", getpid (), fd, fd, sid, sid, len);
	      errno = EINVAL;
	      rv = -1;
	      goto done;
	    }
	  ep.is_ip4 = VPPCOM_IS_IP4;
	  ep.ip = (u8 *) & ((const struct sockaddr_in *) addr)->sin_addr;
	  ep.port = (u16) ((const struct sockaddr_in *) addr)->sin_port;
	  break;

	case AF_INET6:
	  if (len != sizeof (struct sockaddr_in6))
	    {
	      clib_warning
		("LDP<%d>: fd %d (0x%x): ERROR sid %u (0x%x): Invalid "
		 "AF_INET6 addr len %u!", getpid (), fd, fd, sid, sid, len);
	      errno = EINVAL;
	      rv = -1;
	      goto done;
	    }
	  ep.is_ip4 = VPPCOM_IS_IP6;
	  ep.ip = (u8 *) & ((const struct sockaddr_in6 *) addr)->sin6_addr;
	  ep.port = (u16) ((const struct sockaddr_in6 *) addr)->sin6_port;
	  break;

	default:
	  clib_warning ("LDP<%d>: fd %d (0x%x): ERROR sid %u (0x%x): "
			"Unsupported address family %u!",
			getpid (), fd, fd, sid, sid, addr->sa_family);
	  errno = EAFNOSUPPORT;
	  rv = -1;
	  goto done;
	}
      if (VCOM_DEBUG > 0)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): sid %u (0x%x) "
		      "addr %p len %u",
		      getpid (), fd, fd, func_str, sid, sid, addr, len);

      rv = vppcom_session_connect (sid, &ep);
      if (rv != VPPCOM_OK)
	{
	  errno = -rv;
	  rv = -1;
	}
    }
  else
    {
      func_str = "libc_connect";

      if (VCOM_DEBUG > 0)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "addr %p, len %u",
		      getpid (), fd, fd, func_str, addr, len);

      rv = libc_connect (fd, addr, len);
    }

done:
  if (VCOM_DEBUG > 0)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, rv, rv);
    }
  return rv;
}

int
getpeername (int fd, __SOCKADDR_ARG addr, socklen_t * __restrict len)
{
  int rv;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      vppcom_endpt_t ep;
      u8 addr_buf[sizeof (struct in6_addr)];
      u32 size = sizeof (ep);

      ep.ip = addr_buf;
      func_str = "vppcom_session_attr[GET_PEER_ADDR]";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): sid %u (0x%x), "
		      "addr %p, len %u",
		      getpid (), fd, fd, func_str, sid, sid, addr, len);

      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_PEER_ADDR, &ep, &size);
      if (rv != VPPCOM_OK)
	{
	  errno = -rv;
	  rv = -1;
	}
      else
	{
	  rv = vcom_copy_ep_to_sockaddr (addr, len, &ep);
	  if (rv != VPPCOM_OK)
	    {
	      errno = -rv;
	      rv = -1;
	    }
	}
    }
  else
    {
      func_str = "libc_getpeername";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "addr %p, len %u",
		      getpid (), fd, fd, func_str, addr, len);

      rv = libc_getpeername (fd, addr, len);
    }

  if (VCOM_DEBUG > 2)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, rv, rv);
    }
  return rv;
}

ssize_t
send (int fd, const void *buf, size_t n, int flags)
{
  ssize_t size;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {

      func_str = "vppcom_session_sendto";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): sid %u (0x%x), "
		      "buf %p, n %u, flags 0x%x",
		      getpid (), fd, fd, func_str, sid, sid, buf, n, flags);

      size = vppcom_session_sendto (sid, (void *) buf, n, flags, NULL);
      if (size != VPPCOM_OK)
	{
	  errno = -size;
	  size = -1;
	}
    }
  else
    {
      func_str = "libc_send";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "buf %p, n %u, flags 0x%x",
		      getpid (), fd, fd, func_str, buf, n, flags);

      size = libc_send (fd, buf, n, flags);
    }

  if (VCOM_DEBUG > 2)
    {
      if (size < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, size, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, size, size);
    }
  return size;
}

ssize_t
sendfile (int out_fd, int in_fd, off_t * offset, size_t len)
{
  ssize_t size = 0;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (out_fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      int rv;
      ssize_t results = 0;
      size_t n_bytes_left = len;
      size_t bytes_to_read;
      int nbytes;
      int errno_val;
      u8 eagain = 0;
      u32 flags, flags_len = sizeof (flags);

      func_str = "vppcom_session_attr[GET_FLAGS]";
      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_FLAGS, &flags,
				&flags_len);
      if (PREDICT_FALSE (rv != VPPCOM_OK))
	{
	  clib_warning ("LDP<%d>: ERROR: out fd %d (0x%x): %s(): "
			"sid %u (0x%x), returned %d (%s)!", getpid (),
			out_fd, out_fd, func_str, sid, sid, rv,
			vppcom_retval_str (rv));

	  vec_reset_length (vcom->io_buffer);
	  errno = -rv;
	  size = -1;
	  goto done;
	}

      if (offset)
	{
	  off_t off = lseek (in_fd, *offset, SEEK_SET);
	  if (PREDICT_FALSE (off == -1))
	    {
	      func_str = "lseek";
	      errno_val = errno;
	      clib_warning ("LDP<%d>: ERROR: out fd %d (0x%x): %s(): "
			    "SEEK_SET failed: in_fd %d, offset %p, "
			    "*offset %ld, rv %ld, errno %d", getpid (),
			    out_fd, out_fd, in_fd, offset, *offset, off,
			    errno_val);
	      errno = errno_val;
	      size = -1;
	      goto done;
	    }

	  ASSERT (off == *offset);
	}

      do
	{
	  func_str = "vppcom_session_attr[GET_NWRITE]";
	  size = vppcom_session_attr (sid, VPPCOM_ATTR_GET_NWRITE, 0, 0);
	  if (size < 0)
	    {
	      clib_warning
		("LDP<%d>: ERROR: fd %d (0x%x): %s(): sid %u (0x%x), "
		 "returned %d (%s)!", getpid (), out_fd, out_fd, func_str,
		 sid, sid, size, vppcom_retval_str (size));
	      vec_reset_length (vcom->io_buffer);
	      errno = -size;
	      size = -1;
	      goto done;
	    }

	  bytes_to_read = size;
	  if (VCOM_DEBUG > 2)
	    clib_warning
	      ("LDP<%d>: fd %d (0x%x): called %s(): sid %u (0x%x), "
	       "results %ld, n_bytes_left %lu, bytes_to_read %lu", getpid (),
	       out_fd, out_fd, func_str, sid, sid, results, n_bytes_left,
	       bytes_to_read);

	  if (bytes_to_read == 0)
	    {
	      if (flags & O_NONBLOCK)
		{
		  if (!results)
		    {
		      if (VCOM_DEBUG > 2)
			clib_warning ("LDP<%d>: fd %d (0x%x): sid %u (0x%x): "
				      "EAGAIN",
				      getpid (), out_fd, out_fd, sid, sid);
		      eagain = 1;
		    }
		  goto update_offset;
		}
	      else
		continue;
	    }
	  bytes_to_read = clib_min (n_bytes_left, bytes_to_read);
	  vec_validate (vcom->io_buffer, bytes_to_read);
	  nbytes = libc_read (in_fd, vcom->io_buffer, bytes_to_read);
	  if (nbytes < 0)
	    {
	      func_str = "libc_read";
	      errno_val = errno;
	      clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s(): in_fd (%d), "
			    "io_buffer %p, bytes_to_read %lu, rv %d, "
			    "errno %d", getpid (), out_fd, out_fd, func_str,
			    in_fd, vcom->io_buffer, bytes_to_read, nbytes,
			    errno_val);
	      errno = errno_val;

	      if (results == 0)
		{
		  vec_reset_length (vcom->io_buffer);
		  size = -1;
		  goto done;
		}
	      goto update_offset;
	    }
	  func_str = "vppcom_session_write";
	  if (VCOM_DEBUG > 2)
	    clib_warning
	      ("LDP<%d>: fd %d (0x%x): calling %s(): sid %u (0x%x), "
	       "buf %p, nbytes %u: results %d, n_bytes_left %d", getpid (),
	       out_fd, out_fd, func_str, sid, sid, vcom->io_buffer, nbytes,
	       results, n_bytes_left);

	  size = vppcom_session_write (sid, vcom->io_buffer, nbytes);
	  if (size < 0)
	    {
	      if (size == VPPCOM_EAGAIN)
		{
		  if (flags & O_NONBLOCK)
		    {
		      if (!results)
			{
			  if (VCOM_DEBUG > 2)
			    clib_warning
			      ("LDP<%d>: fd %d (0x%x): sid %u (0x%x): "
			       "EAGAIN", getpid (), out_fd, out_fd, sid, sid);
			  eagain = 1;
			}
		      goto update_offset;
		    }
		  else
		    continue;
		}
	      else
		{
		  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s():"
				"sid %u, io_buffer %p, nbytes %u "
				"returned %d (%s)",
				getpid (), out_fd, out_fd, func_str,
				sid, vcom->io_buffer, nbytes,
				size, vppcom_retval_str (size));
		}
	      if (results == 0)
		{
		  vec_reset_length (vcom->io_buffer);
		  errno = -size;
		  size = -1;
		  goto done;
		}
	      goto update_offset;
	    }

	  results += nbytes;
	  ASSERT (n_bytes_left >= nbytes);
	  n_bytes_left = n_bytes_left - nbytes;
	}
      while (n_bytes_left > 0);

    update_offset:
      vec_reset_length (vcom->io_buffer);
      if (offset)
	{
	  off_t off = lseek (in_fd, *offset, SEEK_SET);
	  if (PREDICT_FALSE (off == -1))
	    {
	      func_str = "lseek";
	      errno_val = errno;
	      clib_warning ("LDP<%d>: ERROR: %s(): SEEK_SET failed: "
			    "in_fd %d, offset %p, *offset %ld, "
			    "rv %ld, errno %d", getpid (), in_fd,
			    offset, *offset, off, errno_val);
	      errno = errno_val;
	      size = -1;
	      goto done;
	    }

	  ASSERT (off == *offset);
	  *offset += results + 1;
	}
      if (eagain)
	{
	  errno = EAGAIN;
	  size = -1;
	}
      else
	size = results;
    }
  else
    {
      func_str = "libc_send";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "in_fd %d, offset %p, len %u",
		      getpid (), out_fd, out_fd, func_str,
		      in_fd, offset, len);

      size = libc_sendfile (out_fd, in_fd, offset, len);
    }

done:
  if (VCOM_DEBUG > 2)
    {
      if (size < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), out_fd, out_fd,
			func_str, size, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), out_fd, out_fd, size, size);
    }
  return size;
}

ssize_t
sendfile64 (int out_fd, int in_fd, off_t * offset, size_t len)
{
  return sendfile (out_fd, in_fd, offset, len);
}

ssize_t
recv (int fd, void *buf, size_t n, int flags)
{
  ssize_t size;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      func_str = "vppcom_session_recvfrom";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "sid %u (0x%x), buf %p, n %u, flags 0x%x", getpid (),
		      fd, fd, func_str, sid, sid, buf, n, flags);

      size = vppcom_session_recvfrom (sid, buf, n, flags, NULL);
      if (size < 0)
	{
	  errno = -size;
	  size = -1;
	}
    }
  else
    {
      func_str = "libc_recv";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "buf %p, n %u, flags 0x%x", getpid (),
		      fd, fd, func_str, buf, n, flags);

      size = libc_recv (fd, buf, n, flags);
    }

  if (VCOM_DEBUG > 2)
    {
      if (size < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, size, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, size, size);
    }
  return size;
}

ssize_t
sendto (int fd, const void *buf, size_t n, int flags,
	__CONST_SOCKADDR_ARG addr, socklen_t addr_len)
{
  ssize_t size;
  const char *func_str = __func__;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      vppcom_endpt_t *ep = 0;
      vppcom_endpt_t _ep;

      if (addr)
	{
	  ep = &_ep;
	  ep->vrf = VPPCOM_VRF_DEFAULT;
	  switch (addr->sa_family)
	    {
	    case AF_INET:
	      ep->is_ip4 = VPPCOM_IS_IP4;
	      ep->ip =
		(uint8_t *) & ((const struct sockaddr_in *) addr)->sin_addr;
	      ep->port =
		(uint16_t) ((const struct sockaddr_in *) addr)->sin_port;
	      break;

	    case AF_INET6:
	      ep->is_ip4 = VPPCOM_IS_IP6;
	      ep->ip =
		(uint8_t *) & ((const struct sockaddr_in6 *) addr)->sin6_addr;
	      ep->port =
		(uint16_t) ((const struct sockaddr_in6 *) addr)->sin6_port;
	      break;

	    default:
	      errno = EAFNOSUPPORT;
	      size = -1;
	      goto done;
	    }
	}

      func_str = "vppcom_session_sendto";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "sid %u (0x%x), buf %p, n %u, flags 0x%x, ep %p",
		      getpid (), fd, fd, func_str, sid, sid, buf, n,
		      flags, ep);

      size = vppcom_session_sendto (sid, (void *) buf, n, flags, ep);
      if (size < 0)
	{
	  errno = -size;
	  size = -1;
	}
    }
  else
    {
      func_str = "libc_sendto";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "buf %p, n %u, flags 0x%x, addr %p, addr_len %d",
		      getpid (), fd, fd, func_str, buf, n, flags,
		      addr, addr_len);

      size = libc_sendto (fd, buf, n, flags, addr, addr_len);
    }

done:
  if (VCOM_DEBUG > 2)
    {
      if (size < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, size, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, size, size);
    }
  return size;
}

ssize_t
recvfrom (int fd, void *__restrict buf, size_t n, int flags,
	  __SOCKADDR_ARG addr, socklen_t * __restrict addr_len)
{
  ssize_t size;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      vppcom_endpt_t ep;
      u8 src_addr[sizeof (struct sockaddr_in6)];

      func_str = "vppcom_session_recvfrom";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "sid %u (0x%x), buf %p, n %u, flags 0x%x, ep %p",
		      getpid (), fd, fd, func_str, sid, sid, buf, n,
		      flags, &ep);
      if (addr)
	{
	  ep.ip = src_addr;
	  size = vppcom_session_recvfrom (sid, buf, n, flags, &ep);

	  if (size > 0)
	    size = vcom_copy_ep_to_sockaddr (addr, addr_len, &ep);
	}
      else
	size = vppcom_session_recvfrom (sid, buf, n, flags, NULL);

      if (size < 0)
	{
	  errno = -size;
	  size = -1;
	}
    }
  else
    {
      func_str = "libc_recvfrom";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "buf %p, n %u, flags 0x%x, addr %p, addr_len %d",
		      getpid (), fd, fd, func_str, buf, n, flags,
		      addr, addr_len);

      size = libc_recvfrom (fd, buf, n, flags, addr, addr_len);
    }

  if (VCOM_DEBUG > 2)
    {
      if (size < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, size, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, size, size);
    }
  return size;
}

ssize_t
sendmsg (int fd, const struct msghdr * message, int flags)
{
  ssize_t size;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      func_str = __func__;

      clib_warning ("LDP<%d>: LDP-TBD", getpid ());
      errno = ENOSYS;
      size = -1;
    }
  else
    {
      func_str = "libc_sendmsg";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "message %p, flags 0x%x",
		      getpid (), fd, fd, func_str, message, flags);

      size = libc_sendmsg (fd, message, flags);
    }

  if (VCOM_DEBUG > 2)
    {
      if (size < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, size, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, size, size);
    }
  return size;
}

#ifdef USE_GNU
int
sendmmsg (int fd, struct mmsghdr *vmessages, unsigned int vlen, int flags)
{
  ssize_t size;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      clib_warning ("LDP<%d>: LDP-TBD", getpid ());
      errno = ENOSYS;
      size = -1;
    }
  else
    {
      func_str = "libc_sendmmsg";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "vmessages %p, vlen %u, flags 0x%x",
		      getpid (), fd, fd, func_str, vmessages, vlen, flags);

      size = libc_sendmmsg (fd, vmessages, vlen, flags);
    }

  if (VCOM_DEBUG > 2)
    {
      if (size < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, size, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, size, size);
    }
  return size;
}
#endif

ssize_t
recvmsg (int fd, struct msghdr * message, int flags)
{
  ssize_t size;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      func_str = __func__;

      clib_warning ("LDP<%d>: LDP-TBD", getpid ());
      errno = ENOSYS;
      size = -1;
    }
  else
    {
      func_str = "libc_recvmsg";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "message %p, flags 0x%x",
		      getpid (), fd, fd, func_str, message, flags);

      size = libc_recvmsg (fd, message, flags);
    }

  if (VCOM_DEBUG > 2)
    {
      if (size < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, size, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, size, size);
    }
  return size;
}

#ifdef USE_GNU
int
recvmmsg (int fd, struct mmsghdr *vmessages,
	  unsigned int vlen, int flags, struct timespec *tmo)
{
  ssize_t size;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      clib_warning ("LDP<%d>: LDP-TBD", getpid ());
      errno = ENOSYS;
      size = -1;
    }
  else
    {
      func_str = "libc_recvmmsg";

      if (VCOM_DEBUG > 2)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
		      "vmessages %p, vlen %u, flags 0x%x, tmo %p",
		      getpid (), fd, fd, func_str, vmessages, vlen,
		      flags, tmo);

      size = libc_recvmmsg (fd, vmessages, vlen, flags, tmo);
    }

  if (VCOM_DEBUG > 2)
    {
      if (size < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, size, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, size, size);
    }
  return size;
}
#endif

int
getsockopt (int fd, int level, int optname,
	    void *__restrict optval, socklen_t * __restrict optlen)
{
  int rv;
  const char *func_str = __func__;
  u32 sid = vcom_sid_from_fd (fd);
  u32 buflen = optlen ? (u32) * optlen : 0;

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      rv = -EOPNOTSUPP;

      switch (level)
	{
	case SOL_TCP:
	  switch (optname)
	    {
	    case TCP_NODELAY:
	      func_str = "vppcom_session_attr[SOL_TCP,GET_TCP_NODELAY]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_TCP_NODELAY,
					optval, optlen);
	      break;
	    case TCP_MAXSEG:
	      func_str = "vppcom_session_attr[SOL_TCP,GET_TCP_USER_MSS]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_TCP_USER_MSS,
					optval, optlen);
	      break;
	    case TCP_KEEPIDLE:
	      func_str = "vppcom_session_attr[SOL_TCP,GET_TCP_KEEPIDLE]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_TCP_KEEPIDLE,
					optval, optlen);
	      break;
	    case TCP_KEEPINTVL:
	      func_str = "vppcom_session_attr[SOL_TCP,GET_TCP_KEEPINTVL]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x), SOL_TCP",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_TCP_KEEPINTVL,
					optval, optlen);
	      break;
	    case TCP_INFO:
	      if (optval && optlen && (*optlen == sizeof (struct tcp_info)))
		{
		  if (VCOM_DEBUG > 1)
		    clib_warning ("LDP<%d>: fd %d (0x%x): sid %u (0x%x), "
				  "SOL_TCP, TCP_INFO, optval %p, "
				  "optlen %d: #LDP-NOP#",
				  getpid (), fd, fd, sid, sid,
				  optval, *optlen);
		  memset (optval, 0, *optlen);
		  rv = VPPCOM_OK;
		}
	      else
		rv = -EFAULT;
	      break;
	    default:
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s(): "
			      "sid %u (0x%x), SOL_TCP, "
			      "optname %d unsupported!",
			      getpid (), fd, fd, func_str, sid, sid, optname);
	      break;
	    }
	  break;
	case SOL_IPV6:
	  switch (optname)
	    {
	    case IPV6_V6ONLY:
	      func_str = "vppcom_session_attr[SOL_IPV6,GET_V6ONLY]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_V6ONLY,
					optval, optlen);
	      break;
	    default:
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s(): "
			      "sid %u (0x%x), SOL_IPV6, "
			      "optname %d unsupported!",
			      getpid (), fd, fd, func_str, sid, sid, optname);
	      break;
	    }
	  break;
	case SOL_SOCKET:
	  switch (optname)
	    {
	    case SO_ACCEPTCONN:
	      func_str = "vppcom_session_attr[SOL_SOCKET,GET_ACCEPTCONN]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_LISTEN,
					optval, optlen);
	      break;
	    case SO_KEEPALIVE:
	      func_str = "vppcom_session_attr[SOL_SOCKET,GET_KEEPALIVE]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_KEEPALIVE,
					optval, optlen);
	      break;
	    case SO_PROTOCOL:
	      func_str = "vppcom_session_attr[SOL_SOCKET,GET_PROTOCOL]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_PROTOCOL,
					optval, optlen);
	      *(int *) optval = *(int *) optval ? SOCK_DGRAM : SOCK_STREAM;
	      break;
	    case SO_SNDBUF:
	      func_str = "vppcom_session_attr[SOL_SOCKET,GET_TX_FIFO_LEN]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x), optlen %d",
			      getpid (), fd, fd, func_str, sid, sid, buflen);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_TX_FIFO_LEN,
					optval, optlen);
	      break;
	    case SO_RCVBUF:
	      func_str = "vppcom_session_attr[SOL_SOCKET,GET_RX_FIFO_LEN]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x), optlen %d",
			      getpid (), fd, fd, func_str, sid, sid, buflen);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_RX_FIFO_LEN,
					optval, optlen);
	      break;
	    case SO_REUSEADDR:
	      func_str = "vppcom_session_attr[SOL_SOCKET,GET_REUSEADDR]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_REUSEADDR,
					optval, optlen);
	      break;
	    case SO_BROADCAST:
	      func_str = "vppcom_session_attr[SOL_SOCKET,GET_BROADCAST]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_BROADCAST,
					optval, optlen);
	      break;
	    case SO_ERROR:
	      func_str = "vppcom_session_attr[SOL_SOCKET,GET_ERROR]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_GET_ERROR,
					optval, optlen);
	      break;
	    default:
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s(): "
			      "sid %u (0x%x), SOL_SOCKET, "
			      "optname %d unsupported!",
			      getpid (), fd, fd, func_str, sid, sid, optname);
	      break;
	    }
	  break;
	default:
	  break;
	}

      if (rv != VPPCOM_OK)
	{
	  errno = -rv;
	  rv = -1;
	}
    }
  else
    {
      func_str = "libc_getsockopt";

      if (VCOM_DEBUG > 1)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): level %d, "
		      "optname %d, optval %p, optlen %d",
		      getpid (), fd, fd, func_str, level, optname,
		      optval, optlen);

      rv = libc_getsockopt (fd, level, optname, optval, optlen);
    }

  if (VCOM_DEBUG > 1)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, rv, rv);
    }
  return rv;
}

int
setsockopt (int fd, int level, int optname,
	    const void *optval, socklen_t optlen)
{
  int rv;
  const char *func_str = __func__;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      rv = -EOPNOTSUPP;

      switch (level)
	{
	case SOL_TCP:
	  switch (optname)
	    {
	    case TCP_NODELAY:
	      func_str = "vppcom_session_attr[SOL_TCP,SET_TCP_NODELAY]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_SET_TCP_NODELAY,
					(void *) optval, &optlen);
	      break;
	    case TCP_MAXSEG:
	      func_str = "vppcom_session_attr[SOL_TCP,SET_TCP_USER_MSS]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_SET_TCP_USER_MSS,
					(void *) optval, &optlen);
	      break;
	    case TCP_KEEPIDLE:
	      func_str = "vppcom_session_attr[SOL_TCP,SET_TCP_KEEPIDLE]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_SET_TCP_KEEPIDLE,
					(void *) optval, &optlen);
	      break;
	    case TCP_KEEPINTVL:
	      func_str = "vppcom_session_attr[SOL_TCP,SET_TCP_KEEPINTVL]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x), SOL_TCP",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_SET_TCP_KEEPINTVL,
					(void *) optval, &optlen);
	      break;
	    default:
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s(): "
			      "sid %u (0x%x), SOL_TCP, "
			      "optname %d unsupported!",
			      getpid (), fd, fd, func_str, sid, sid, optname);
	      break;
	    }
	  break;
	case SOL_IPV6:
	  switch (optname)
	    {
	    case IPV6_V6ONLY:
	      func_str = "vppcom_session_attr[SOL_IPV6,SET_V6ONLY]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_SET_V6ONLY,
					(void *) optval, &optlen);
	      break;
	    default:
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s(): "
			      "sid %u (0x%x), SOL_IPV6, "
			      "optname %d unsupported!",
			      getpid (), fd, fd, func_str, sid, sid, optname);
	      break;
	    }
	  break;
	case SOL_SOCKET:
	  switch (optname)
	    {
	    case SO_KEEPALIVE:
	      func_str = "vppcom_session_attr[SOL_SOCKET,SET_KEEPALIVE]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_SET_KEEPALIVE,
					(void *) optval, &optlen);
	      break;
	    case SO_REUSEADDR:
	      func_str = "vppcom_session_attr[SOL_SOCKET,SET_REUSEADDR]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_SET_REUSEADDR,
					(void *) optval, &optlen);
	      break;
	    case SO_BROADCAST:
	      func_str = "vppcom_session_attr[SOL_SOCKET,SET_BROADCAST]";
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): "
			      "sid %u (0x%x)",
			      getpid (), fd, fd, func_str, sid, sid);
	      rv = vppcom_session_attr (sid, VPPCOM_ATTR_SET_BROADCAST,
					(void *) optval, &optlen);
	      break;
	    default:
	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s(): "
			      "sid %u (0x%x), SOL_SOCKET, "
			      "optname %d unsupported!",
			      getpid (), fd, fd, func_str, sid, sid, optname);
	      break;
	    }
	  break;
	default:
	  break;
	}

      if (rv != VPPCOM_OK)
	{
	  errno = -rv;
	  rv = -1;
	}
    }
  else
    {
      func_str = "libc_setsockopt";

      if (VCOM_DEBUG > 1)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): level %d, "
		      "optname %d, optval %p, optlen %d",
		      getpid (), fd, fd, func_str, level, optname,
		      optval, optlen);

      rv = libc_setsockopt (fd, level, optname, optval, optlen);
    }

  if (VCOM_DEBUG > 1)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, rv, rv);
    }
  return rv;
}

int
listen (int fd, int n)
{
  int rv;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      func_str = "vppcom_session_listen";

      if (VCOM_DEBUG > 0)
	clib_warning
	  ("LDP<%d>: fd %d (0x%x): calling %s(): sid %u (0x%x), n %d",
	   getpid (), fd, fd, func_str, sid, sid, n);

      rv = vppcom_session_listen (sid, n);
      if (rv != VPPCOM_OK)
	{
	  errno = -rv;
	  rv = -1;
	}
    }
  else
    {
      func_str = "libc_listen";

      if (VCOM_DEBUG > 0)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): n %d",
		      getpid (), fd, fd, func_str, n);

      rv = libc_listen (fd, n);
    }

  if (VCOM_DEBUG > 0)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, rv, rv);
    }
  return rv;
}

static inline int
vcom_accept4 (int listen_fd, __SOCKADDR_ARG addr,
	      socklen_t * __restrict addr_len, int flags)
{
  int rv;
  const char *func_str;
  u32 listen_sid = vcom_sid_from_fd (listen_fd);
  int accept_sid;

  if ((errno = -vcom_init ()))
    return -1;

  if (listen_sid != INVALID_SESSION_ID)
    {
      vppcom_endpt_t ep;
      u8 src_addr[sizeof (struct sockaddr_in6)];
      memset (&ep, 0, sizeof (ep));
      ep.ip = src_addr;

      func_str = "vppcom_session_accept";

      if (VCOM_DEBUG > 0)
	clib_warning ("LDP<%d>: listen fd %d (0x%x): calling %s(): "
		      "listen sid %u (0x%x), ep %p, flags 0x%x",
		      getpid (), listen_fd, listen_fd, func_str,
		      listen_sid, listen_sid, ep, flags);

      accept_sid = vppcom_session_accept (listen_sid, &ep, flags);
      if (accept_sid < 0)
	{
	  errno = -accept_sid;
	  rv = -1;
	}
      else
	{
	  rv = vcom_copy_ep_to_sockaddr (addr, addr_len, &ep);
	  if (rv != VPPCOM_OK)
	    {
	      (void) vppcom_session_close ((u32) accept_sid);
	      errno = -rv;
	      rv = -1;
	    }
	  else
	    {
	      func_str = "vcom_fd_from_sid";
	      if (VCOM_DEBUG > 0)
		clib_warning ("LDP<%d>: listen fd %d (0x%x): calling %s(): "
			      "accept sid %u (0x%x), ep %p, flags 0x%x",
			      getpid (), listen_fd, listen_fd,
			      func_str, accept_sid, accept_sid, ep, flags);
	      rv = vcom_fd_from_sid ((u32) accept_sid);
	      if (rv < 0)
		{
		  (void) vppcom_session_close ((u32) accept_sid);
		  errno = -rv;
		  rv = -1;
		}
	    }
	}
    }
  else
    {
      func_str = "libc_accept4";

      if (VCOM_DEBUG > 0)
	clib_warning ("LDP<%d>: listen fd %d (0x%x): calling %s(): "
		      "addr %p, addr_len %p, flags 0x%x",
		      getpid (), listen_fd, listen_fd, func_str,
		      addr, addr_len, flags);

      rv = libc_accept4 (listen_fd, addr, addr_len, flags);
    }

  if (VCOM_DEBUG > 0)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: listen fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), listen_fd,
			listen_fd, func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: listen fd %d (0x%x): returning %d (0x%x)",
		      getpid (), listen_fd, listen_fd, rv, rv);
    }
  return rv;
}

int
accept4 (int fd, __SOCKADDR_ARG addr, socklen_t * __restrict addr_len,
	 int flags)
{
  return vcom_accept4 (fd, addr, addr_len, flags);
}

int
accept (int fd, __SOCKADDR_ARG addr, socklen_t * __restrict addr_len)
{
  return vcom_accept4 (fd, addr, addr_len, 0);
}

int
shutdown (int fd, int how)
{
  int rv;
  const char *func_str;
  u32 sid = vcom_sid_from_fd (fd);

  if ((errno = -vcom_init ()))
    return -1;

  if (sid != INVALID_SESSION_ID)
    {
      func_str = __func__;

      clib_warning ("LDP<%d>: LDP-TBD", getpid ());
      errno = ENOSYS;
      rv = -1;
    }
  else
    {
      func_str = "libc_shutdown";

      if (VCOM_DEBUG > 1)
	clib_warning ("LDP<%d>: fd %d (0x%x): calling %s(): how %d",
		      getpid (), fd, fd, func_str, how);

      rv = libc_shutdown (fd, how);
    }

  if (VCOM_DEBUG > 1)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, rv, rv);
    }
  return rv;
}

int
epoll_create1 (int flags)
{
  const char *func_str;
  int rv;

  if ((errno = -vcom_init ()))
    return -1;

  func_str = "vppcom_epoll_create";

  if (VCOM_DEBUG > 1)
    clib_warning ("LDP<%d>: calling %s()", getpid (), func_str);

  rv = vppcom_epoll_create ();

  if (PREDICT_FALSE (rv < 0))
    {
      errno = -rv;
      rv = -1;
    }
  else
    rv = vcom_fd_from_sid ((u32) rv);

  if (VCOM_DEBUG > 1)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: %s() failed! "
			"rv %d, errno = %d",
			getpid (), func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: returning epfd %d (0x%x)", getpid (), rv, rv);
    }
  return rv;
}

int
epoll_create (int size)
{
  return epoll_create1 (0);
}

int
epoll_ctl (int epfd, int op, int fd, struct epoll_event *event)
{
  int rv;
  const char *func_str;
  u32 vep_idx = vcom_sid_from_fd (epfd);

  if ((errno = -vcom_init ()))
    return -1;

  if (vep_idx != INVALID_SESSION_ID)
    {
      u32 sid = vcom_sid_from_fd (fd);

      if (sid != INVALID_SESSION_ID)
	{
	  func_str = "vppcom_epoll_create";

	  if (VCOM_DEBUG > 1)
	    clib_warning ("LDP<%d>: epfd %d (0x%x): calling %s(): "
			  "vep_idx %d (0x%x), op %d, sid %u (0x%x), event %p",
			  getpid (), epfd, epfd, func_str, vep_idx, vep_idx,
			  sid, sid, event);

	  rv = vppcom_epoll_ctl (vep_idx, op, sid, event);
	  if (rv != VPPCOM_OK)
	    {
	      errno = -rv;
	      rv = -1;
	    }
	}
      else
	{
	  int epfd;
	  u32 size = sizeof (epfd);

	  func_str = "vppcom_session_attr[GET_LIBC_EPFD]";
	  epfd = vppcom_session_attr (vep_idx, VPPCOM_ATTR_GET_LIBC_EPFD,
				      0, 0);
	  if (!epfd)
	    {
	      func_str = "libc_epoll_create1";

	      if (VCOM_DEBUG > 1)
		clib_warning ("LDP<%d>: calling %s(): EPOLL_CLOEXEC",
			      getpid (), func_str);

	      epfd = libc_epoll_create1 (EPOLL_CLOEXEC);
	      if (epfd < 0)
		{
		  rv = epfd;
		  goto done;
		}

	      func_str = "vppcom_session_attr[SET_LIBC_EPFD]";
	      rv = vppcom_session_attr (vep_idx, VPPCOM_ATTR_SET_LIBC_EPFD,
					&epfd, &size);
	      if (rv < 0)
		{
		  errno = -rv;
		  rv = -1;
		  goto done;
		}
	    }
	  else if (PREDICT_FALSE (epfd < 0))
	    {
	      errno = -epfd;
	      rv = -1;
	      goto done;
	    }

	  rv = libc_epoll_ctl (epfd, op, fd, event);
	}
    }
  else
    {
      func_str = "libc_epoll_ctl";

      if (VCOM_DEBUG > 1)
	clib_warning ("LDP<%d>: epfd %d (0x%x): calling %s(): "
		      "op %d, fd %d (0x%x), event %p",
		      getpid (), epfd, epfd, func_str, op, fd, fd, event);

      rv = libc_epoll_ctl (epfd, op, fd, event);
    }

done:
  if (VCOM_DEBUG > 1)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: fd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), fd, fd,
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: fd %d (0x%x): returning %d (0x%x)",
		      getpid (), fd, fd, rv, rv);
    }
  return rv;
}

static inline int
vcom_epoll_pwait (int epfd, struct epoll_event *events,
		  int maxevents, int timeout, const sigset_t * sigmask)
{
  const char *func_str;
  int rv = 0;
  double time_to_wait = (double) 0;
  double time_out, now = 0;
  u32 vep_idx = vcom_sid_from_fd (epfd);
  int libc_epfd;

  if ((errno = -vcom_init ()))
    return -1;

  if (PREDICT_FALSE (!events || (timeout < -1)))
    {
      errno = EFAULT;
      return -1;
    }

  if (PREDICT_FALSE (vep_idx == INVALID_SESSION_ID))
    {
      clib_warning ("LDP<%d>: ERROR: epfd %d (0x%x): bad vep_idx %d (0x%x)!",
		    getpid (), epfd, epfd, vep_idx, vep_idx);
      errno = EBADFD;
      return -1;
    }

  time_to_wait = ((timeout >= 0) ? (double) timeout / (double) 1000 : 0);
  time_out = clib_time_now (&vcom->clib_time) + time_to_wait;

  func_str = "vppcom_session_attr[GET_LIBC_EPFD]";
  libc_epfd = vppcom_session_attr (vep_idx, VPPCOM_ATTR_GET_LIBC_EPFD, 0, 0);
  if (PREDICT_FALSE (libc_epfd < 0))
    {
      errno = -libc_epfd;
      rv = -1;
      goto done;
    }

  if (VCOM_DEBUG > 2)
    clib_warning ("LDP<%d>: epfd %d (0x%x): vep_idx %d (0x%x), "
		  "libc_epfd %d (0x%x), events %p, maxevents %d, "
		  "timeout %d, sigmask %p", getpid (), epfd, epfd,
		  vep_idx, vep_idx, libc_epfd, libc_epfd, events,
		  maxevents, timeout, sigmask);
  do
    {
      if (!vcom->epoll_wait_vcl)
	{
	  func_str = "vppcom_epoll_wait";

	  if (VCOM_DEBUG > 3)
	    clib_warning ("LDP<%d>: epfd %d (0x%x): calling %s(): "
			  "vep_idx %d (0x%x), events %p, maxevents %d",
			  getpid (), epfd, epfd, func_str,
			  vep_idx, vep_idx, events, maxevents);

	  rv = vppcom_epoll_wait (vep_idx, events, maxevents, 0);
	  if (rv > 0)
	    {
	      vcom->epoll_wait_vcl = 1;
	      goto done;
	    }
	  else if (rv < 0)
	    {
	      errno = -rv;
	      rv = -1;
	      goto done;
	    }
	}
      else
	vcom->epoll_wait_vcl = 0;

      if (libc_epfd > 0)
	{
	  func_str = "libc_epoll_pwait";

	  if (VCOM_DEBUG > 3)
	    clib_warning ("LDP<%d>: epfd %d (0x%x): calling %s(): "
			  "libc_epfd %d (0x%x), events %p, "
			  "maxevents %d, sigmask %p",
			  getpid (), epfd, epfd, func_str,
			  libc_epfd, libc_epfd, events, maxevents, sigmask);

	  rv = libc_epoll_pwait (libc_epfd, events, maxevents, 1, sigmask);
	  if (rv != 0)
	    goto done;
	}

      if (timeout != -1)
	now = clib_time_now (&vcom->clib_time);
    }
  while (now < time_out);

done:
  if (VCOM_DEBUG > 3)
    {
      if (libc_epfd > 0)
	epfd = libc_epfd;
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: epfd %d (0x%x): %s() failed! "
			"rv %d, errno = %d", getpid (), epfd, epfd,
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	clib_warning ("LDP<%d>: epfd %d (0x%x): returning %d (0x%x)",
		      getpid (), epfd, epfd, rv, rv);
    }
  return rv;
}

int
epoll_pwait (int epfd, struct epoll_event *events,
	     int maxevents, int timeout, const sigset_t * sigmask)
{
  return vcom_epoll_pwait (epfd, events, maxevents, timeout, sigmask);
}

int
epoll_wait (int epfd, struct epoll_event *events, int maxevents, int timeout)
{
  return vcom_epoll_pwait (epfd, events, maxevents, timeout, NULL);
}

int
poll (struct pollfd *fds, nfds_t nfds, int timeout)
{
  const char *func_str = __func__;
  int rv, i, n_libc_fds, n_revents;
  u32 sid;
  vcl_poll_t *vp;
  double wait_for_time;

  if (VCOM_DEBUG > 3)
    clib_warning ("LDP<%d>: fds %p, nfds %d, timeout %d",
		  getpid (), fds, nfds, timeout);

  if (timeout >= 0)
    wait_for_time = (f64) timeout / 1000;
  else
    wait_for_time = -1;

  n_libc_fds = 0;
  for (i = 0; i < nfds; i++)
    {
      if (fds[i].fd >= 0)
	{
	  if (VCOM_DEBUG > 3)
	    clib_warning ("LDP<%d>: fds[%d].fd %d (0x%0x), .events = 0x%x, "
			  ".revents = 0x%x", getpid (), i, fds[i].fd,
			  fds[i].fd, fds[i].events, fds[i].revents);

	  sid = vcom_sid_from_fd (fds[i].fd);
	  if (sid != INVALID_SESSION_ID)
	    {
	      fds[i].fd = -fds[i].fd;
	      vec_add2 (vcom->vcl_poll, vp, 1);
	      vp->fds_ndx = i;
	      vp->sid = sid;
	      vp->events = fds[i].events;
#ifdef __USE_XOPEN2K
	      if (fds[i].events & POLLRDNORM)
		vp->events |= POLLIN;
	      if (fds[i].events & POLLWRNORM)
		vp->events |= POLLOUT;
#endif
	      vp->revents = &fds[i].revents;
	    }
	  else
	    n_libc_fds++;
	}
    }

  n_revents = 0;
  do
    {
      if (vec_len (vcom->vcl_poll))
	{
	  func_str = "vppcom_poll";

	  if (VCOM_DEBUG > 3)
	    clib_warning ("LDP<%d>: calling %s(): "
			  "vcl_poll %p, n_sids %u (0x%x): "
			  "n_libc_fds %u",
			  getpid (), func_str, vcom->vcl_poll,
			  vec_len (vcom->vcl_poll), vec_len (vcom->vcl_poll),
			  n_libc_fds);

	  rv = vppcom_poll (vcom->vcl_poll, vec_len (vcom->vcl_poll), 0);
	  if (rv < 0)
	    {
	      errno = -rv;
	      rv = -1;
	      goto done;
	    }
	  else
	    n_revents += rv;
	}

      if (n_libc_fds)
	{
	  func_str = "libc_poll";

	  if (VCOM_DEBUG > 3)
	    clib_warning ("LDP<%d>: calling %s(): fds %p, nfds %u: n_sids %u",
			  getpid (), fds, nfds, vec_len (vcom->vcl_poll));

	  rv = libc_poll (fds, nfds, 0);
	  if (rv < 0)
	    goto done;
	  else
	    n_revents += rv;
	}

      if (n_revents)
	{
	  rv = n_revents;
	  goto done;
	}
    }
  while ((wait_for_time == -1) ||
	 (clib_time_now (&vcom->clib_time) < wait_for_time));
  rv = 0;

done:
  vec_foreach (vp, vcom->vcl_poll)
  {
    fds[vp->fds_ndx].fd = -fds[vp->fds_ndx].fd;
#ifdef __USE_XOPEN2K
    if ((fds[vp->fds_ndx].revents & POLLIN) &&
	(fds[vp->fds_ndx].events & POLLRDNORM))
      fds[vp->fds_ndx].revents |= POLLRDNORM;
    if ((fds[vp->fds_ndx].revents & POLLOUT) &&
	(fds[vp->fds_ndx].events & POLLWRNORM))
      fds[vp->fds_ndx].revents |= POLLWRNORM;
#endif
  }
  vec_reset_length (vcom->vcl_poll);

  if (VCOM_DEBUG > 3)
    {
      if (rv < 0)
	{
	  int errno_val = errno;
	  perror (func_str);
	  clib_warning ("LDP<%d>: ERROR: %s() failed! "
			"rv %d, errno = %d", getpid (),
			func_str, rv, errno_val);
	  errno = errno_val;
	}
      else
	{
	  clib_warning ("LDP<%d>: returning %d (0x%x): n_sids %u, "
			"n_libc_fds %d", getpid (), rv, rv,
			vec_len (vcom->vcl_poll), n_libc_fds);

	  for (i = 0; i < nfds; i++)
	    {
	      if (fds[i].fd >= 0)
		{
		  if (VCOM_DEBUG > 3)
		    clib_warning ("LDP<%d>: fds[%d].fd %d (0x%0x), "
				  ".events = 0x%x, .revents = 0x%x",
				  getpid (), i, fds[i].fd, fds[i].fd,
				  fds[i].events, fds[i].revents);
		}
	    }
	}
    }

  return rv;
}

#ifdef USE_GNU
int
ppoll (struct pollfd *fds, nfds_t nfds,
       const struct timespec *timeout, const sigset_t * sigmask)
{
  if ((errno = -vcom_init ()))
    return -1;

  clib_warning ("LDP<%d>: LDP-TBD", getpid ());
  errno = ENOSYS;


  return -1;
}
#endif

void CONSTRUCTOR_ATTRIBUTE vcom_constructor (void);

void DESTRUCTOR_ATTRIBUTE vcom_destructor (void);

/*
 * This function is called when the library is loaded
 */
void
vcom_constructor (void)
{
  swrap_constructor ();
  if (vcom_init () != 0)
    fprintf (stderr, "\nLDP<%d>: ERROR: vcom_constructor: failed!\n",
	     getpid ());
  else
    clib_warning ("LDP<%d>: VCOM constructor: done!\n", getpid ());
}

/*
 * This function is called when the library is unloaded
 */
void
vcom_destructor (void)
{
  swrap_destructor ();
  if (vcom->init)
    {
      vppcom_app_destroy ();
      vcom->init = 0;
    }

  /* Don't use clib_warning() here because that calls writev()
   * which will call vcom_init().
   */
  printf ("%s:%d: LDP<%d>: VCOM destructor: done!\n",
	  __func__, __LINE__, getpid ());
}


/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
