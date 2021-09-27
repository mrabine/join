/**
 * MIT License
 *
 * Copyright (c) 2021 Mathieu Rabine
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef __JOIN_SOCKETIO_HPP__
#define __JOIN_SOCKETIO_HPP__

// libjoin.
#include <join/socket.hpp>

// C++.
#include <streambuf>
#include <utility>
#include <memory>
#include <chrono>

namespace join
{
    /**
     * @brief .
     */
    template <class Protocol>
    class BasicSocketStreambuf : public std::streambuf
    {
    public:
        using Socket = BasicStreamSocket <Protocol>;
        using Endpoint = typename Protocol::Endpoint;

        /**
         * @brief default constructor.
         */
        BasicSocketStreambuf ()
        : _socket (Socket::Mode::NonBlocking)
        {
        }

        /**
         * @brief copy constructor.
         * @param other other object to copy.
         */
        BasicSocketStreambuf (const BasicSocketStreambuf& other) = delete;

        /**
         * @brief move constructor.
         * @param other other object to move.
         */
        BasicSocketStreambuf (BasicSocketStreambuf&& other)
        : std::streambuf (std::move (other)),
          _allocated (other._allocated),
          _gsize (other._gsize),
          _psize (other._psize),
          _buf (other._buf),
          _timeout (other._timeout),
          _socket (std::move (other._socket))
        {
            other._allocated = false;
            other._gsize = BUFSIZ / 2;
            other._psize = BUFSIZ / 2;
            other._buf = nullptr;
            other._timeout = 30000;
        }

        /**
         * @brief copy assignment operator.
         * @param other other object to assign.
         * @return assigned object.
         */
        BasicSocketStreambuf& operator= (const BasicSocketStreambuf& other) = delete;

        /**
         * @brief move assignment operator.
         * @param other other object to assign.
         * @return assigned object.
         */
        BasicSocketStreambuf& operator= (BasicSocketStreambuf&& other)
        {
            this->close ();

            std::streambuf::operator= (std::move (other));

            _allocated = other._allocated;
            _gsize = other._gsize;
            _psize = other._psize;
            _buf = other._buf;
            _timeout = other._timeout;
            _socket = std::move (other._socket);

            other._allocated = false;
            other._gsize = BUFSIZ / 2;
            other._psize = BUFSIZ / 2;
            other._buf = nullptr;
            other._timeout = 30000;

            return *this;
        }

        /**
         * @brief destroy the socket instance.
         */
        virtual ~BasicSocketStreambuf ()
        {
            if (_buf)
            {
                this->overflow (traits_type::eof ());
            }
        }

        /**
         * @brief make a connection to the given endpoint.
         * @param endpoint endpoint to connect to.
         * @return this on success, nullptr on failure.
         */
        BasicSocketStreambuf* connect (const Endpoint& endpoint)
        {
            if (_socket.close () == -1)
            {
                return nullptr;
            }

            this->allocateBuffer ();

            if (_socket.connect (endpoint) == -1)
            {
                if (lastError != Errc::TemporaryError)
                {
                    return nullptr;
                }

                if (!_socket.waitConnected (_timeout))
                {
                    return nullptr;
                }
            }

            return this;
        }

        /**
         * @brief close the connection.
         * @return this on success, nullptr on failure.
         */
        BasicSocketStreambuf* close ()
        {
            BasicSocketStreambuf* result = this;

            if (_buf && (this->overflow (traits_type::eof ()) == traits_type::eof ()))
            {
                result = nullptr;
            }

            if (_socket.close () == -1)
            {
                result = nullptr;
            }

            this->freeBuffer ();

            return result;
        }

        /**
         * @brief set the socket timeout.
         * @param ms timeout in milliseconds.
         */
        void timeout (int ms)
        {
            _timeout = ms;
        }

        /**
         * @brief get the current timeout in milliseconds.
         * @return the current timeout.
         */
        int timeout () const
        {
            return _timeout;
        }

        /**
         * @brief get the nested socket.
         * @return the nested socket.
         */
        Socket& socket ()
        {
            return _socket;
        }

    protected:
        /**
         * @brief .
         * @return .
         */
        virtual int_type underflow () override
        {
            if (!_socket.connected ())
            {
                lastError = make_error_code (Errc::OperationFailed);
                return traits_type::eof ();
            }

            if (this->eback () == nullptr)
            {
                this->setg (_buf, _buf, _buf);
            }

            if (this->gptr () == this->egptr ())
            {
                for (;;)
                {
                    int nread = _socket.read (this->eback (), _gsize);
                    if (nread == -1)
                    {
                        if (lastError == Errc::TemporaryError)
                        {
                            if (_socket.waitReadyRead (_timeout))
                            {
                                continue;
                            }

                            _socket.close();
                        }

                        return traits_type::eof ();
                    }

                    this->setg (this->eback (), this->eback (), this->eback () + nread);
                    break;
                }
            }

            return traits_type::to_int_type (*this->gptr ());
        }

        /**
         * @brief .
         * @param c .
         * @return .
         */
        virtual int_type pbackfail (int_type c = traits_type::eof ()) override
        {
            if (this->eback () < this->gptr ())
            {
                this->gbump (-1);

                if (traits_type::eq_int_type (c, traits_type::eof ()))
                {
                    return traits_type::not_eof (c);
                }

                if (!traits_type::eq (traits_type::to_char_type (c), this->gptr ()[-1]))
                {
                    *this->gptr () = traits_type::to_char_type (c);
                }

                return c;
            }

            return traits_type::eof ();
        }

        /**
         * @brief .
         * @param c .
         * @return .
         */
        virtual int_type overflow (int_type c = traits_type::eof ()) override
        {
            if (!_socket.connected ())
            {
                lastError = make_error_code (Errc::OperationFailed);
                return traits_type::eof ();
            }

            if (this->pbase () == nullptr)
            {
                this->setp (_buf + _gsize, _buf + _gsize + _psize);
            }

            if ((this->pptr () < this->epptr ()) && (c != traits_type::eof ()))
            {
                return this->sputc (traits_type::to_char_type (c));
            }

            std::streamsize pending = this->pptr () - this->pbase ();
            std::streamsize n = 0;

            while (n < pending)
            {
                int nwrite = _socket.write (this->pbase () + n, pending - n);
                if (nwrite == -1)
                {
                    if (lastError == Errc::TemporaryError)
                    {
                        if (_socket.waitReadyWrite (_timeout))
                        {
                            continue;
                        }

                        _socket.close();
                    }

                    return traits_type::eof ();
                }

                n += nwrite;
            }

            this->setp (this->pbase (), this->pbase () + _psize);

            if (c == traits_type::eof ())
            {
                return traits_type::not_eof (c);
            }

            return this->sputc (traits_type::to_char_type (c));
        }

        /**
         * @brief .
         * @return .
         */
        virtual int_type sync () override
        {
            if (_buf)
            {
                return this->overflow (traits_type::eof ());
            }

            return traits_type::not_eof (traits_type::eof ());
        }

        /**
         * @brief .
         * @param s .
         * @param n .
         * @return .
         */
        virtual std::streambuf* setbuf (char* s, std::streamsize n) override
        {
            if (!_buf)
            {
                if ((s == nullptr) && (n == 0))
                {
                    _gsize = 1;
                    _psize = 1;
                }
                else
                {
                    auto d = std::ldiv (n, 2);
                    _gsize = d.quot + d.rem;
                    _psize = d.quot;
                    _buf = s;
                }
            }

            return this;
        }

        /**
         * @brief allocate internal buffer.
         */
        void allocateBuffer ()
        {
            if (!_buf)
            {
                _buf = new char [BUFSIZ];
                _allocated = true;
            }
        }

        /**
         * @brief free internal buffer.
         */
        void freeBuffer ()
        {
            if (_allocated)
            {
                delete [] _buf;
                _allocated = false;
                _buf = nullptr;
            }
        }

        /// internal buffer status.
        bool _allocated = false;

        /// internal buffer size.
        std::streamsize _gsize = BUFSIZ / 2;
        std::streamsize _psize = BUFSIZ / 2;

        /// internal buffer.
        char* _buf = nullptr;

        /// timeout.
        int _timeout = 30000;

        /// internal socket.
        Socket _socket;
    };

    /**
     * @brief .
     */
    template <class Protocol>
    class BasicSocketIoStream : public std::iostream
    {
    public:
        using Socket = BasicStreamSocket <Protocol>;
        using SocketStreambuf = BasicSocketStreambuf <Protocol>;
        using Endpoint = typename Protocol::Endpoint;

        /**
         * @brief .
         */
        BasicSocketIoStream ()
        : std::iostream (&_streambuf)
        {
            this->setf (std::ios_base::unitbuf);
        }

        /**
         * @brief .
         * @param other .
         */
        BasicSocketIoStream (const BasicSocketIoStream& other) = delete;

        /**
         * @brief .
         * @param other .
         */
        BasicSocketIoStream (BasicSocketIoStream&& other)
        : std::iostream (std::move (other)),
          _streambuf (std::move (other._streambuf))
        {
            this->set_rdbuf (&_streambuf);
        }

        /**
         * @brief .
         * @param other .
         */
        BasicSocketIoStream& operator=(const BasicSocketIoStream& other) = delete;

        /**
         * @brief .
         * @param other .
         */
        BasicSocketIoStream& operator=(BasicSocketIoStream&& other)
        {
            std::iostream::operator= (std::move (other));

            _streambuf = std::move (other._streambuf);

            return *this;
        }

        virtual ~BasicSocketIoStream () = default;

        /**
         * @brief .
         * @param endpoint .
         * @throw std::ios_base::failure.
         */
        void connect (const Endpoint& endpoint)
        {
            if (this->rdbuf ()->connect (endpoint) == nullptr)
            {
                this->setstate (std::ios_base::failbit);
            }
        }

        /**
         * @brief .
         * @throw std::ios_base::failure.
         */
        void close ()
        {
            if (this->rdbuf ()->close () == nullptr)
            {
                this->setstate (std::ios_base::failbit);
            }
        }

        /**
         * @brief .
         * @return .
         */
        SocketStreambuf* rdbuf () const
        {
            return const_cast <SocketStreambuf*> (std::addressof (_streambuf));
        }

        /**
         * @brief .
         * @param other .
         */
        void timeout (int ms)
        {
            this->rdbuf ()->timeout (ms);
        }

        /**
         * @brief .
         * @return .
         */
        int timeout () const
        {
            return this->rdbuf ()->timeout ();
        }

        /**
         * @brief .
         * @return .
         */
        Socket& socket ()
        {
            return this->rdbuf ()->socket ();
        }

    private:
        /// .
        SocketStreambuf _streambuf;
    };
}

#endif
