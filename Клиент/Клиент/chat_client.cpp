//
// chat_client.cpp
// ~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2016 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#define _CRT_SECURE_NO_WARNINGS
#include <cstdlib>
#include <deque>
#include <iostream>
#include <thread>
#include <boost/asio.hpp>
#include "chat_message.hpp"
#include<string>

using boost::asio::ip::tcp;

typedef std::deque<chat_message> chat_message_queue;

std::string userName, key;

class Cryptor
{
	std::string key;
public:
	void encrypt(char* s)
	{
		int len = strlen(s);
		for (int i = 0, a = 0; (a < len); ++s, ++a)
		{
			*s += key[i];
			if (i == (key.size() - 1))
				i = 0;
			else
				++i;
		}
	}
	void decrypt(char* s)
	{
		int len = strlen(s);
		for (int i = 0, a = 0; (a < len); ++s, ++a)
		{
			*s -= key[i];
			if (i == (key.size() - 1))
				i = 0;
			else
				++i;
		}
	}
	Cryptor(std::string k) : key(k) {};
};

Cryptor *cr;

class chat_client
{
public:
	chat_client(boost::asio::io_service& io_service,
		tcp::resolver::iterator endpoint_iterator)
		: io_service_(io_service),
		socket_(io_service)
	{
		do_connect(endpoint_iterator);
	}

	void write(const chat_message& msg)
	{
		io_service_.post(
			[this, msg]()
		{
			bool write_in_progress = !write_msgs_.empty();
			write_msgs_.push_back(msg);
			if (!write_in_progress)
			{
				do_write();
			}
		});
	}

	void close()
	{
		io_service_.post([this]() { socket_.close(); });
	}

private:
	void do_connect(tcp::resolver::iterator endpoint_iterator)
	{
		boost::asio::async_connect(socket_, endpoint_iterator,
			[this](boost::system::error_code ec, tcp::resolver::iterator)
		{
			if (!ec)
			{
				do_read_header();
			}
		});
	}

	void do_read_header()
	{
		boost::asio::async_read(socket_,
			boost::asio::buffer(read_msg_.data(), chat_message::header_length),
			[this](boost::system::error_code ec, std::size_t /*length*/)
		{
			if (!ec && read_msg_.decode_header())
			{
				do_read_body();
			}
			else
			{
				socket_.close();
			}
		});
	}

	void do_read_body()
	{
		boost::asio::async_read(socket_,
			boost::asio::buffer(read_msg_.body(), read_msg_.body_length()),
			[this](boost::system::error_code ec, std::size_t /*length*/)
		{
			if (!ec)
			{
				cr->decrypt(read_msg_.body());
				if (!strncmp(read_msg_.body(), userName.c_str(), userName.size()))
					do_read_header();
				else
				{
					std::cout.write(read_msg_.body(), read_msg_.body_length());
					std::cout << "\n";
					do_read_header();
				}
			}
			else
			{
				socket_.close();
			}
		});
	}

	void do_write()
	{
		boost::asio::async_write(socket_,
			boost::asio::buffer(write_msgs_.front().data(),
				write_msgs_.front().length()),
			[this](boost::system::error_code ec, std::size_t /*length*/)
		{
			if (!ec)
			{
				write_msgs_.pop_front();
				if (!write_msgs_.empty())
				{
					do_write();
				}
			}
			else
			{
				socket_.close();
			}
		});
	}

private:
	boost::asio::io_service& io_service_;
	tcp::socket socket_;
	chat_message read_msg_;
	chat_message_queue write_msgs_;
};

int main(int argc, char* argv[])
{
	try
	{
		if (argc != 3)
		{
			std::cerr << "Usage: chat_client <host> <port>\n";
			return 1;
		}

		// Запрос имени пользователя
		std::cout << "Enter username\n";
		std::getline(std::cin, userName);

		std::cout << "Enter encryption key word\n";
		std::getline(std::cin, key);

		cr = new Cryptor(key);

		//bool enteredChat = false; // Сообщает, выведено ли сообщение о том, что пользователь зашёл в чат

		boost::asio::io_service io_service;

		tcp::resolver resolver(io_service);
		auto endpoint_iterator = resolver.resolve({ argv[1], argv[2] });
		chat_client c(io_service, endpoint_iterator);

		std::thread t([&io_service]() { io_service.run(); });

		char line[chat_message::max_body_length + 1];

		/* Отправка стартового сообщения */

		int j = 0;
		for (auto s : (userName + ": joined chat!"))
		{
			line[j] = s;
			line[j + 1] = '\0';
			++j;
		}
		cr->encrypt(line);
		chat_message msg;
		msg.body_length(std::strlen(line));
		std::memcpy(msg.body(), line, msg.body_length());
		msg.encode_header();
		c.write(msg);

		while (std::cin.getline(line, chat_message::max_body_length + 1))
		{
			// Вставка имени пользователя
			std::string res_str = userName + ':' + ' ';
			for (int i = 0; i < std::strlen(line); ++i)
				res_str += line[i];
			for (int i = 0; i < res_str.size(); ++i)
				line[i] = res_str[i];
			line[res_str.size()] = '\0';
			/*if (!strcmp(line, (userName + ':' + ' ').c_str()) && !enteredChat)
			{
			res_str += "joined chat!";
			for (int i = 0; i < res_str.size(); ++i)
			line[i] = res_str[i];
			line[res_str.size()] = '\0';
			enteredChat = true;
			}
			else
			if (!strcmp(line, (userName + ':' + ' ').c_str()) && enteredChat)
			continue;*/

			cr->encrypt(line);

			chat_message msg;
			msg.body_length(std::strlen(line));
			std::memcpy(msg.body(), line, msg.body_length());
			msg.encode_header();
			c.write(msg);
		}

		c.close();
		t.join();
	}
	catch (std::exception& e)
	{
		std::cerr << "Exception: " << e.what() << "\n";
	}

	return 0;
}