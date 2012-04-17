/*
 * \brief  Loader service
 * \author Norman Feske
 * \date   2012-04-17
 */

/*
 * Copyright (C) 2010-2012 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

/* Genode includes */
#include <base/env.h>
#include <base/rpc_server.h>
#include <base/signal.h>
#include <base/sleep.h>
#include <loader_session/loader_session.h>
#include <cap_session/connection.h>
#include <nitpicker_view/capability.h>
#include <root/component.h>

/* local includes */
#include <child.h>
#include <nitpicker.h>
#include <rom.h>


namespace Loader {

	using namespace Genode;


	class Session_component : public Rpc_object<Session>
	{
		private:

			struct Local_rom_service : Service
			{
				Rpc_entrypoint             &_ep;
				Parent_service              _parent_rom_service;
				Rom_module_registry        &_rom_modules;
				Lock                        _lock;
				List<Rom_session_component> _rom_sessions;

				void _close(Rom_session_component *rom)
				{
					_ep.dissolve(rom);
					destroy(env()->heap(), rom);
					_rom_sessions.remove(rom);
				}

				Local_rom_service(Rpc_entrypoint      &ep,
				                  Rom_module_registry &rom_modules)
				:
					Service("virtual_rom"),
					_ep(ep),
					_parent_rom_service(Rom_session::service_name()),
					_rom_modules(rom_modules)
				{ }

				~Local_rom_service()
				{
					Lock::Guard guard(_lock);

					while (_rom_sessions.first()) {
						_close(_rom_sessions.first()); }
				}

				Genode::Session_capability session(const char *args)
				{
					/* try to find ROM module at local ROM service */
					try {
						Lock::Guard guard(_lock);

						char name[Session::Name::MAX_SIZE];
						
						/* extract filename from session arguments */
						Arg_string::find_arg(args, "filename")
							.string(name, sizeof(name), "");

						Rom_module &module = _rom_modules.lookup_and_lock(name);

						Rom_session_component *rom = new (env()->heap())
							Rom_session_component(module);

						_rom_sessions.insert(rom);

						return _ep.manage(rom);

					} catch (...) { }

					/* fall back to parent_rom_service */
					return _parent_rom_service.session(args);
				}

				void close(Session_capability session)
				{
					Lock::Guard guard(_lock);

					Rpc_object_base *rom = _ep.obj_by_cap(session);

					Rom_session_component *component =
						dynamic_cast<Rom_session_component *>(rom);

					if (component) {
						_close(component);
						return;
					}

					_parent_rom_service.close(session);
				}

				void upgrade(Session_capability session, const char *) { }
			};

			struct Local_nitpicker_service : Service
			{
				Rpc_entrypoint &ep;

				Signal_context_capability view_ready_sigh;

				Nitpicker::Session_component *open_session;

				Local_nitpicker_service(Rpc_entrypoint &ep)
				:
					Service("virtual_nitpicker"), ep(ep), open_session(0)
				{ }

				~Local_nitpicker_service()
				{
					if (!open_session)
						return;

					ep.dissolve(open_session);
					destroy(env()->heap(), open_session);
				}

				Genode::Session_capability session(const char *args)
				{
					if (open_session)
						throw Unavailable();

					/*
					 * XXX replace allocation from 'env()->heap()' with
					 *     use of session-specific allocator
					 */
					open_session = new (env()->heap())
						Nitpicker::Session_component(ep, view_ready_sigh, args);

					return ep.manage(open_session);
				}

				void upgrade(Genode::Session_capability session, const char *) { }
			};

			enum { STACK_SIZE = 2*4096 };

			size_t                    _ram_quota;
			size_t                    _subsystem_ram_quota_limit;
			int                       _width, _height;
			Rpc_entrypoint            _ep;
			Service_registry          _parent_services;
			Rom_module_registry       _rom_modules;
			Local_rom_service         _rom_service;
			Local_nitpicker_service   _nitpicker_service;
			Child                    *_child;

			/**
			 * Return virtual nitpicker session component
			 */
			Nitpicker::Session_component *_virtual_nitpicker_session()
			{
				if (!_nitpicker_service.open_session)
					throw View_does_not_exist();

				return _nitpicker_service.open_session;
			}

		public:

			/**
			 * Constructor
			 */
			Session_component(size_t quota, Ram_session &ram, Cap_session &cap)
			:
				_ram_quota(quota),
				_subsystem_ram_quota_limit(~0),
				_width(-1), _height(-1),
				_ep(&cap, STACK_SIZE, "session_ep"),
				_rom_modules(ram, *env()->heap()), /* XXX remove env()->heap() */
				_rom_service(_ep, _rom_modules),
				_nitpicker_service(_ep),
				_child(0)
			{ }

			~Session_component()
			{
				if (_child)
					destroy(env()->heap(), _child);
			}


			/******************************
			 ** Loader session interface **
			 ******************************/

			Dataspace_capability alloc_rom_module(Name const &name, size_t size)
			{
				return _rom_modules.alloc_rom_module(name.string(), size);
			}

			void commit_rom_module(Name const &name)
			{
				try {
					_rom_modules.commit_rom_module(name.string()); }
				catch (Rom_module_registry::Lookup_failed) {
					throw Rom_module_does_not_exist(); }
			}

			void ram_quota(size_t quantum)
			{
				_subsystem_ram_quota_limit = quantum;
			}

			void constrain_geometry(int width, int height)
			{
				_width = width, _height = height;
			}

			void view_ready_sigh(Signal_context_capability sigh)
			{
				_nitpicker_service.view_ready_sigh = sigh;
			}

			void start(Name const &binary_name, Name const &label)
			{
				if (_child) {
					PWRN("cannot start subsystem twice");
					return;
				}

				/*
				 * XXX account for ROM modules
				 */
				size_t const ram_quota = _subsystem_ram_quota_limit;

				/*
				 * XXX don't use 'env()->heap()'
				 */
				_child = new (env()->heap())
					Child(binary_name.string(), label.string(), _ep,
					      ram_quota, _parent_services, _rom_service,
					      _nitpicker_service, _width, _height);
			}

			Nitpicker::View_capability view()
			{
				return _virtual_nitpicker_session()->loader_view();
			}

			View_geometry view_geometry()
			{
				return _virtual_nitpicker_session()->loader_view_geometry();
			}
	};


	class Root : public Root_component<Session_component>
	{
		private:

			Ram_session &_ram;
			Cap_session &_cap;

		protected:

			Session_component *_create_session(const char *args)
			{
				size_t quota =
					Arg_string::find_arg(args, "ram_quota").long_value(0);

				return new (md_alloc()) Session_component(quota, _ram, _cap);
			}

		public:

			/**
			 * Constructor
			 *
			 * \param session_ep  entry point for managing ram session objects
			 * \param md_alloc    meta-data allocator to be used by root
			 *                    component
			 */
			Root(Rpc_entrypoint &session_ep, Allocator &md_alloc,
			     Ram_session &ram, Cap_session &cap)
			:
				Root_component<Session_component>(&session_ep, &md_alloc),
				_ram(ram), _cap(cap)
			{ }
	};
}


int main()
{
	using namespace Genode;

	enum { STACK_SIZE = 8*1024 };
	static Cap_connection cap;
	static Rpc_entrypoint ep(&cap, STACK_SIZE, "loader_ep");

	static Loader::Root root(ep, *env()->heap(), *env()->ram_session(), cap);

	env()->parent()->announce(ep.manage(&root));

	sleep_forever();
	return 0;
}
