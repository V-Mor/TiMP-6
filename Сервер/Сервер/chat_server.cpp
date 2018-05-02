//
// chat_server.cpp
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
#include <list>
#include <memory>
#include <set>
#include <utility>
#include <boost/asio.hpp>
#include<string>
#include "chat_message.hpp"

using boost::asio::ip::tcp;

//----------------------------------------------------------------------

typedef std::deque<chat_message> chat_message_queue;

//----------------------------------------------------------------------

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

class chat_participant
{
public:
	virtual ~chat_participant() {}
	virtual void deliver(const chat_message& msg) = 0;
	char participant_name[64];
};

typedef std::shared_ptr<chat_participant> chat_participant_ptr;

//----------------------------------------------------------------------

class chat_room
{
public:
	void join(chat_participant_ptr participant)
	{
		participants_.insert(participant);
		for (auto msg : recent_msgs_)
			participant->deliver(msg);
	}

	void leave(chat_participant_ptr participant)
	{
		participants_.erase(participant);
	}

	void deliver(const chat_message& msg)
	{
		recent_msgs_.push_back(msg);
		while (recent_msgs_.size() > max_recent_msgs)
			recent_msgs_.pop_front();

		for (auto participant : participants_)
			participant->deliver(msg);
	}

	std::string getKey()
	{
		return key;
	}

	void setKey(std::string k)
	{
		key = k;
	}

	std::set<chat_participant_ptr> getParticipants()
	{
		return participants_;
	}

private:
	std::set<chat_participant_ptr> participants_;
	enum { max_recent_msgs = 100 };
	chat_message_queue recent_msgs_;
	std::string key;
};

//----------------------------------------------------------------------

class chat_session
	: public chat_participant,
	public std::enable_shared_from_this<chat_session>
{
public:
	chat_session(tcp::socket socket, chat_room& room)
		: socket_(std::move(socket)),
		room_(room)
	{
	}

	void start()
	{
		room_.join(shared_from_this());
		do_read_header();
	}

	void deliver(const chat_message& msg)
	{
		bool write_in_progress = !write_msgs_.empty();
		write_msgs_.push_back(msg);
		if (!write_in_progress)
		{
			do_write();
		}
	}

private:

	chat_participant_ptr findClient(const char* name)
	{
		for (auto client : room_.getParticipants())
			if (!strcmp(((client)->participant_name), name))
				return client;
		return nullptr;
	}

	void do_read_header()
	{
		auto self(shared_from_this());
		boost::asio::async_read(socket_,
			boost::asio::buffer(read_msg_.data(), chat_message::header_length),
			[this, self](boost::system::error_code ec, std::size_t /*length*/)
		{
			if (!ec && read_msg_.decode_header())
			{
				do_read_body();
			}
			else
			{
				room_.leave(shared_from_this());
			}
		});
	}

	void do_read_body()
	{
		auto self(shared_from_this());
		boost::asio::async_read(socket_,
			boost::asio::buffer(read_msg_.body(), read_msg_.body_length()),
			[this, self](boost::system::error_code ec, std::size_t /*length*/)
		{
			if (!ec)
			{
				if (!nameSpecified)			// Если имя не определено, задать имя
				{
					Cryptor kr(room_.getKey());
					kr.decrypt(read_msg_.body());
					int i(0);
					while ((read_msg_.body())[i] != ':')
					{
						participant_name[i] = (read_msg_.body())[i];
						participant_name[i + 1] = '\0';
						++i;
					}
					kr.encrypt(read_msg_.body());
					nameSpecified = true;
				}

				char clientName[64];				// Имя адресата
				Cryptor kr(room_.getKey());
				kr.decrypt(read_msg_.body());
				if (!strncmp(read_msg_.body() + strlen(participant_name) + 2, "/TO", 3))
				{
					int i = 0;
					for (char* c = read_msg_.body() + strlen(participant_name) + 6; *c != ':'; ++c, ++i)
					{
						clientName[i] = *c;
						clientName[i + 1] = '\0';
					}
					kr.encrypt(read_msg_.body());
					if (findClient(clientName))
					{
						(findClient(clientName))->deliver(read_msg_);	// Ищем клиента с нужным именем и отправляем только ему
						do_read_header();
					}
					else 
					{
						std::memcpy(read_msg_.body(), "Client not found!\0", read_msg_.body_length());
						(findClient(participant_name))->deliver(read_msg_);
						do_read_header();
					}
				}
				else
				{
					kr.encrypt(read_msg_.body());
					room_.deliver(read_msg_);
					do_read_header();
				}
			}
			else
			{
				room_.leave(shared_from_this());
			}
		});
	}

	void do_write()
	{
		auto self(shared_from_this());
		boost::asio::async_write(socket_,
			boost::asio::buffer(write_msgs_.front().data(),
				write_msgs_.front().length()),
			[this, self](boost::system::error_code ec, std::size_t /*length*/)
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
				room_.leave(shared_from_this());
			}
		});
	}

	bool nameSpecified = false;
	tcp::socket socket_;
	chat_room& room_;
	chat_message read_msg_;
	chat_message_queue write_msgs_;
};

//----------------------------------------------------------------------

class chat_server
{
public:
	chat_server(boost::asio::io_service& io_service,
		const tcp::endpoint& endpoint, std::string key)
		: acceptor_(io_service, endpoint),
		socket_(io_service)
	{
		room_.setKey(key);
		do_accept();
	}

private:
	void do_accept()
	{
		acceptor_.async_accept(socket_,
			[this](boost::system::error_code ec)
		{
			if (!ec)
			{
				std::make_shared<chat_session>(std::move(socket_), room_)->start();
			}

			do_accept();
		});
	}

	tcp::acceptor acceptor_;
	tcp::socket socket_;
	chat_room room_;
};

//----------------------------------------------------------------------

int main(int argc, char* argv[])
{
	try
	{
		if (argc < 2)
		{
			std::cerr << "Usage: chat_server <port> [<port> ...]\n";
			return 1;
		}

		boost::asio::io_service io_service;

		std::list<chat_server> servers;
		for (int i = 1; i < argc; ++i)
		{
			std::string key;
			std::cout << "Enter keyword for chatroom #" << i << std::endl;
			std::cout.flush();
			std::getline(std::cin, key);
			tcp::endpoint endpoint(tcp::v4(), std::atoi(argv[i]));
			servers.emplace_back(io_service, endpoint, key);
		}

		io_service.run();
	}
	catch (std::exception& e)
	{
		std::cerr << "Exception: " << e.what() << "\n";
	}

	return 0;
}