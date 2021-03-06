/*
 * \brief  Child representation
 * \author Norman Feske
 * \date   2010-05-04
 */

/*
 * Copyright (C) 2010-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* local includes */
#include <child.h>


Init::Child::Apply_config_result
Init::Child::apply_config(Xml_node start_node)
{
	if (_state == STATE_ABANDONED)
		return NO_SIDE_EFFECTS;

	/*
	 * If the child's environment is incomplete, restart it to attempt
	 * the re-routing of its environment sessions.
	 */
	if (!_child.active()) {
		abandon();
		return MAY_HAVE_SIDE_EFFECTS;
	}

	bool provided_services_changed = false;

	enum Config_update { CONFIG_APPEARED, CONFIG_VANISHED,
	                     CONFIG_CHANGED,  CONFIG_UNCHANGED };

	Config_update config_update = CONFIG_UNCHANGED;

	/* import new start node if new version differs */
	if (start_node.size() != _start_node->xml().size() ||
	    Genode::memcmp(start_node.addr(), _start_node->xml().addr(),
	                   start_node.size()) != 0)
	{
		/*
		 * Check for a change of the version attribute, force restart
		 * if the version changed.
		 */
		if (_version != start_node.attribute_value("version", Version())) {
			abandon();
			return MAY_HAVE_SIDE_EFFECTS;
		}

		/*
		 * Start node changed
		 *
		 * Determine how the inline config is affected.
		 */
		char const * const tag = "config";
		bool const config_was_present = _start_node->xml().has_sub_node(tag);
		bool const config_is_present  = start_node.has_sub_node(tag);

		if (config_was_present && !config_is_present)
			config_update = CONFIG_VANISHED;

		if (!config_was_present && config_is_present)
			config_update = CONFIG_APPEARED;

		if (config_was_present && config_is_present) {

			Xml_node old_config = _start_node->xml().sub_node(tag);
			Xml_node new_config = start_node.sub_node(tag);

			if (Genode::memcmp(old_config.addr(), new_config.addr(),
			                   min(old_config.size(), new_config.size())))
				config_update = CONFIG_CHANGED;
		}

		/*
		 * Import updated <provides> node
		 *
		 * First abandon services that are no longer present in the
		 * <provides> node. Then add services that have newly appeared.
		 */
		_child_services.for_each([&] (Routed_service &service) {

			if (!_provided_by_this(service))
				return;

			typedef Service::Name Name;
			Name const name = service.name();

			bool still_provided = false;
			_provides_sub_node(start_node)
				.for_each_sub_node("service", [&] (Xml_node node) {
					if (name == node.attribute_value("name", Name()))
						still_provided = true; });

			if (!still_provided) {
				service.abandon();
				provided_services_changed = true;
			}
		});

		_provides_sub_node(start_node).for_each_sub_node("service",
		                                                 [&] (Xml_node node) {
			if (_service_exists(node))
				return;

			_add_service(node);
			provided_services_changed = true;
		});

		/*
		 * Import new binary name. A change may affect the route for
		 * the binary's ROM session, triggering the restart of the
		 * child.
		 */
		_binary_name = _binary_from_xml(start_node, _unique_name);

		/* import new start node */
		_start_node.construct(_alloc, start_node);
	}

	/*
	 * Apply change to '_config_rom_service'. This will
	 * potentially result in a change of the "config" ROM route, which
	 * may in turn prompt the routing-check below to abandon (restart)
	 * the child.
	 */
	switch (config_update) {
	case CONFIG_UNCHANGED:                                       break;
	case CONFIG_CHANGED:  _config_rom_service->trigger_update(); break;
	case CONFIG_APPEARED: _config_rom_service.construct(*this);  break;
	case CONFIG_VANISHED: _config_rom_service->abandon();        break;
	}

	/* validate that the routes of all existing sessions remain intact */
	{
		bool routing_changed = false;
		_child.for_each_session([&] (Session_state const &session) {
			if (!_route_valid(session))
				routing_changed = true; });

		if (routing_changed) {
			abandon();
			return MAY_HAVE_SIDE_EFFECTS;
		}
	}

	if (provided_services_changed)
		return MAY_HAVE_SIDE_EFFECTS;

	return NO_SIDE_EFFECTS;
}


static Init::Ram_quota assigned_ram_from_start_node(Genode::Xml_node start_node)
{
	using namespace Init;

	size_t assigned = 0;

	start_node.for_each_sub_node("resource", [&] (Xml_node resource) {
		if (resource.attribute_value("name", String<8>()) == "RAM")
			assigned = resource.attribute_value("quantum", Number_of_bytes()); });

	return Ram_quota { assigned };
}


void Init::Child::apply_ram_upgrade()
{
	Ram_quota const assigned_ram_quota = assigned_ram_from_start_node(_start_node->xml());

	if (assigned_ram_quota.value <= _resources.assigned_ram_quota.value)
		return;

	size_t const increase = assigned_ram_quota.value
	                      - _resources.assigned_ram_quota.value;
	size_t const limit    = _ram_limit_accessor.ram_limit().value;
	size_t const transfer = min(increase, limit);

	if (increase > limit)
		warning(name(), ": assigned RAM exceeds available RAM");

	/*
	 * Remember assignment and apply RAM upgrade to child
	 *
	 * Note that we remember the actually transferred amount as the
	 * assigned amount. In the case where the value is clamped to to
	 * the limit, the value as given in the config remains diverged
	 * from the assigned value. This way, a future config update will
	 * attempt the completion of the upgrade if memory become
	 * available.
	 */
	if (transfer) {
		_resources.assigned_ram_quota =
			Ram_quota { _resources.assigned_ram_quota.value + transfer };

		_check_ram_constraints(_ram_limit_accessor.ram_limit());

		ref_pd().transfer_quota(_child.pd_session_cap(), Ram_quota{transfer});

		/* wake up child that blocks on a resource request */
		if (_requested_resources.constructed()) {
			_child.notify_resource_avail();
			_requested_resources.destruct();
		}
	}
}


void Init::Child::apply_ram_downgrade()
{
	Ram_quota const assigned_ram_quota = assigned_ram_from_start_node(_start_node->xml());

	if (assigned_ram_quota.value >= _resources.assigned_ram_quota.value)
		return;

	size_t const decrease = _resources.assigned_ram_quota.value
	                      - assigned_ram_quota.value;

	/*
	 * The child may concurrently consume quota from its RAM session,
	 * causing the 'transfer_quota' to fail. For this reason, we repeatedly
	 * attempt the transfer.
	 */
	unsigned max_attempts = 4, attempts = 0;
	for (; attempts < max_attempts; attempts++) {

		/* give up if the child's available RAM is exhausted */
		size_t const preserved = 16*1024;
		size_t const avail     = _child.ram().avail_ram().value;

		if (avail < preserved)
			break;

		size_t const transfer = min(avail - preserved, decrease);

		try {
			_child.pd().transfer_quota(ref_pd_cap(), Ram_quota{transfer});
			_resources.assigned_ram_quota =
				Ram_quota { _resources.assigned_ram_quota.value - transfer };
			break;
		} catch (...) { }
	}

	if (attempts == max_attempts)
		warning(name(), ": RAM downgrade failed after ", max_attempts, " attempts");

	/*
	 * If designated RAM quota is lower than the child's consumed RAM, issue
	 * a yield request to the child.
	 */
	if (assigned_ram_quota.value < _resources.assigned_ram_quota.value) {

		size_t const demanded = _resources.assigned_ram_quota.value
		                      - assigned_ram_quota.value;

		Parent::Resource_args const args { "ram_quota=", Number_of_bytes(demanded) };
		_child.yield(args);
	}

}


void Init::Child::report_state(Xml_generator &xml, Report_detail const &detail) const
{
	xml.node("child", [&] () {

		xml.attribute("name",   _unique_name);
		xml.attribute("binary", _binary_name);

		if (_version.valid())
			xml.attribute("version", _version);

		if (detail.ids())
			xml.attribute("id", _id.value);

		if (!_child.active())
			xml.attribute("state", "incomplete");

		if (_exited)
			xml.attribute("exited", _exit_value);

		if (detail.child_ram() && _child.ram_session_cap().valid()) {
			xml.node("ram", [&] () {

				xml.attribute("assigned", String<32> {
					Number_of_bytes(_resources.assigned_ram_quota.value) });

				generate_ram_info(xml, _child.ram());

				if (_requested_resources.constructed() && _requested_resources->ram.value)
					xml.attribute("requested", String<32>(_requested_resources->ram));
			});
		}

		if (detail.child_caps() && _child.pd_session_cap().valid()) {
			xml.node("caps", [&] () {

				xml.attribute("assigned", String<32>(_resources.assigned_cap_quota));

				generate_caps_info(xml, _child.pd());

				if (_requested_resources.constructed() && _requested_resources->caps.value)
					xml.attribute("requested", String<32>(_requested_resources->caps));
			});
		}

		Session_state::Detail const
			session_detail { detail.session_args() ? Session_state::Detail::ARGS
			                                       : Session_state::Detail::NO_ARGS};

		if (detail.requested()) {
			xml.node("requested", [&] () {
				_child.for_each_session([&] (Session_state const &session) {
					xml.node("session", [&] () {
						session.generate_client_side_info(xml, session_detail); }); }); });
		}

		if (detail.provided()) {
			xml.node("provided", [&] () {

				auto fn = [&] (Session_state const &session) {
					xml.node("session", [&] () {
						session.generate_server_side_info(xml, session_detail); }); };

				_session_requester.id_space().for_each<Session_state const>(fn);
			});
		}
	});
}


void Init::Child::init(Pd_session &session, Pd_session_capability cap)
{
	session.ref_account(_env.pd_session_cap());

	size_t const initial_session_costs =
		session_alloc_batch_size()*_child.session_factory().session_costs();

	Ram_quota const ram_quota { _resources.effective_ram_quota().value > initial_session_costs
	                          ? _resources.effective_ram_quota().value - initial_session_costs
	                          : 0 };

	Cap_quota const cap_quota { _resources.effective_cap_quota().value };

	try { _env.pd().transfer_quota(cap, cap_quota); }
	catch (Out_of_caps) {
		error(name(), ": unable to initialize cap quota of PD"); }

	try { _env.ram().transfer_quota(cap, ram_quota); }
	catch (Out_of_ram) {
		error(name(), ": unable to initialize RAM quota of PD"); }
}


void Init::Child::init(Cpu_session &session, Cpu_session_capability cap)
{
	static size_t avail = Cpu_session::quota_lim_upscale(                    100, 100);
	size_t const   need = Cpu_session::quota_lim_upscale(_resources.cpu_quota_pc, 100);
	size_t need_adj = 0;

	if (need > avail || avail == 0) {
		warn_insuff_quota(Cpu_session::quota_lim_downscale(avail, 100));
		need_adj = Cpu_session::quota_lim_upscale(100, 100);
		avail    = 0;
	} else {
		need_adj = Cpu_session::quota_lim_upscale(need, avail);
		avail   -= need;
	}
	session.ref_account(_env.cpu_session_cap());
	_env.cpu().transfer_quota(cap, need_adj);
}


Init::Child::Route Init::Child::resolve_session_request(Service::Name const &service_name,
                                                        Session_label const &label)
{
	/* check for "config" ROM request */
	if (service_name == Rom_session::service_name() &&
	    label.last_element() == "config") {

		if (_config_rom_service.constructed() &&
		   !_config_rom_service->abandoned())
			return Route { _config_rom_service->service(), label,
			               Session::Diag{false} };

		/*
		 * \deprecated  the support for the <configfile> tag will
		 *              be removed
		 */
		if (_start_node->xml().has_sub_node("configfile")) {

			typedef String<50> Name;
			Name const rom =
				_start_node->xml().sub_node("configfile")
				                  .attribute_value("name", Name());

			/* prevent infinite recursion */
			if (rom == "config") {
				error("configfile must not be named 'config'");
				throw Service_denied();
			}

			return resolve_session_request(service_name,
			                               prefixed_label(name(), rom));
		}

		/*
		 * If there is neither an inline '<config>' nor a
		 * '<configfile>' node present, we apply the regular session
		 * routing to the "config" ROM request.
		 */
	}

	/*
	 * Check for the binary's ROM request
	 *
	 * The binary is requested as a ROM with the child's unique
	 * name ('Child_policy::binary_name' equals 'Child_policy::name').
	 * If the binary name differs from the child's unique name,
	 * we resolve the session request with the binary name as label.
	 * Otherwise the regular routing is applied.
	 */
	if (service_name == Rom_session::service_name() &&
	    label == _unique_name && _unique_name != _binary_name)
		return resolve_session_request(service_name, _binary_name);

	/* check for "session_requests" ROM request */
	if (service_name == Rom_session::service_name()
	 && label.last_element() == Session_requester::rom_name())
		return Route { _session_requester.service(),
		               Session::Label(), Session::Diag{false} };

	try {
		Xml_node route_node = _default_route_accessor.default_route();
		try {
			route_node = _start_node->xml().sub_node("route"); }
		catch (...) { }
		Xml_node service_node = route_node.sub_node();

		for (; ; service_node = service_node.next()) {

			bool service_wildcard = service_node.has_type("any-service");

			if (!service_node_matches(service_node, label, name(), service_name))
				continue;

			Xml_node target = service_node.sub_node();
			for (; ; target = target.next()) {

				/*
				 * Determine session label to be provided to the server
				 *
				 * By default, the client's identity (accompanied with the a
				 * client-provided label) is presented as session label to the
				 * server. However, the target node can explicitly override the
				 * client's identity by a custom label via the 'label'
				 * attribute.
				 */
				typedef String<Session_label::capacity()> Label;
				Label const target_label =
					target.attribute_value("label", Label(label.string()));

				Session::Diag const
					target_diag { target.attribute_value("diag", false) };

				auto no_filter = [] (Service &) -> bool { return false; };

				if (target.has_type("parent")) {

					try {
						return Route { find_service(_parent_services, service_name, no_filter),
						               target_label, target_diag };
					} catch (Service_denied) { }
				}

				if (target.has_type("child")) {

					typedef Name_registry::Name Name;
					Name server_name = target.attribute_value("name", Name());
					server_name = _name_registry.deref_alias(server_name);

					auto filter_server_name = [&] (Routed_service &s) -> bool {
						return s.child_name() != server_name; };

					try {
						return Route { find_service(_child_services, service_name, filter_server_name),
						               target_label, target_diag };

					} catch (Service_denied) { }
				}

				if (target.has_type("any-child")) {

					if (is_ambiguous(_child_services, service_name)) {
						error(name(), ": ambiguous routes to "
						      "service \"", service_name, "\"");
						throw Service_denied();
					}
					try {
						return Route { find_service(_child_services, service_name, no_filter),
						               target_label, target_diag };

					} catch (Service_denied) { }
				}

				if (!service_wildcard) {
					warning(name(), ": lookup for service \"", service_name, "\" failed");
					throw Service_denied();
				}

				if (target.last())
					break;
			}
		}
	} catch (Xml_node::Nonexistent_sub_node) { }

	warning(name(), ": no route to service \"", service_name, "\"");
	throw Service_denied();
}


void Init::Child::filter_session_args(Service::Name const &service,
                                      char *args, size_t args_len)
{
	/*
	 * Intercept CPU session requests to scale priorities
	 */
	if (service == Cpu_session::service_name() && _prio_levels_log2 > 0) {

		unsigned long priority = Arg_string::find_arg(args, "priority").ulong_value(0);

		/* clamp priority value to valid range */
		priority = min((unsigned)Cpu_session::PRIORITY_LIMIT - 1, priority);

		long discarded_prio_lsb_bits_mask = (1 << _prio_levels_log2) - 1;
		if (priority & discarded_prio_lsb_bits_mask)
			warning("priority band too small, losing least-significant priority bits");

		priority >>= _prio_levels_log2;

		/* assign child priority to the most significant priority bits */
		priority |= _priority*(Cpu_session::PRIORITY_LIMIT >> _prio_levels_log2);

		/* override priority when delegating the session request to the parent */
		String<64> value { Hex(priority) };
		Arg_string::set_arg(args, args_len, "priority", value.string());
	}

	/*
	 * Remove phys_start and phys_size RAM-session arguments unless
	 * explicitly permitted by the child configuration.
	 */
	if (service == Pd_session::service_name()) {

		/*
		 * If the child is allowed to constrain physical memory allocations,
		 * pass the child-provided constraints as session arguments to core.
		 * If no constraints are specified, we apply the constraints for
		 * allocating DMA memory (as the only use case for the constrain-phys
		 * mechanism).
		 */
		if (_constrain_phys) {
			addr_t start = 0;
			addr_t size  = (sizeof(long) == 4) ? 0xc0000000UL : 0x100000000UL;

			Arg_string::find_arg(args, "phys_start").ulong_value(start);
			Arg_string::find_arg(args, "phys_size") .ulong_value(size);

			Arg_string::set_arg(args, args_len, "phys_start", String<32>(Hex(start)).string());
			Arg_string::set_arg(args, args_len, "phys_size",  String<32>(Hex(size)) .string());
		} else {
			Arg_string::remove_arg(args, "phys_start");
			Arg_string::remove_arg(args, "phys_size");
		}
	}
}


Genode::Affinity Init::Child::filter_session_affinity(Affinity const &session_affinity)
{
	Affinity::Space    const &child_space    = _resources.affinity.space();
	Affinity::Location const &child_location = _resources.affinity.location();

	/* check if no valid affinity space was specified */
	if (session_affinity.space().total() == 0)
		return Affinity(child_space, child_location);

	Affinity::Space    const &session_space    = session_affinity.space();
	Affinity::Location const &session_location = session_affinity.location();

	/* scale resolution of resulting space */
	Affinity::Space space(child_space.multiply(session_space));

	/* subordinate session affinity to child affinity subspace */
	Affinity::Location location(child_location
	                            .multiply_position(session_space)
	                            .transpose(session_location.xpos(),
	                                       session_location.ypos()));

	return Affinity(space, location);
}


void Init::Child::announce_service(Service::Name const &service_name)
{
	if (_verbose.enabled())
		log("child \"", name(), "\" announces service \"", service_name, "\"");

	bool found = false;
	_child_services.for_each([&] (Routed_service &service) {
		if (service.has_id_space(_session_requester.id_space())
		 && service.name() == service_name)
			found = true; });

	if (!found)
		error(name(), ": illegal announcement of "
		      "service \"", service_name, "\"");
}


void Init::Child::resource_request(Parent::Resource_args const &args)
{
	log("child \"", name(), "\" requests resources: ", args);

	_requested_resources.construct(args);
	_report_update_trigger.trigger_report_update();
}


Init::Child::Child(Env                      &env,
                   Allocator                &alloc,
                   Verbose            const &verbose,
                   Id                        id,
                   Report_update_trigger    &report_update_trigger,
                   Xml_node                  start_node,
                   Default_route_accessor   &default_route_accessor,
                   Default_caps_accessor    &default_caps_accessor,
                   Name_registry            &name_registry,
                   Ram_quota                 ram_limit,
                   Cap_quota                 cap_limit,
                   Ram_limit_accessor       &ram_limit_accessor,
                   Prio_levels               prio_levels,
                   Affinity::Space const    &affinity_space,
                   Registry<Parent_service> &parent_services,
                   Registry<Routed_service> &child_services)
:
	_env(env), _alloc(alloc), _verbose(verbose), _id(id),
	_report_update_trigger(report_update_trigger),
	_list_element(this),
	_start_node(_alloc, start_node),
	_default_route_accessor(default_route_accessor),
	_ram_limit_accessor(ram_limit_accessor),
	_name_registry(name_registry),
	_resources(_resources_from_start_node(start_node, prio_levels, affinity_space,
	                                      default_caps_accessor.default_caps(), cap_limit)),
	_resources_checked((_check_ram_constraints(ram_limit),
	                    _check_cap_constraints(cap_limit), true)),
	_parent_services(parent_services),
	_child_services(child_services),
	_session_requester(_env.ep().rpc_ep(), _env.ram(), _env.rm())
{
	if (_verbose.enabled()) {
		log("child \"",       _unique_name, "\"");
		log("  RAM quota:  ", _resources.effective_ram_quota());
		log("  cap quota:  ", _resources.effective_cap_quota());
		log("  ELF binary: ", _binary_name);
		log("  priority:   ", _resources.priority);
	}

	/*
	 * Determine services provided by the child
	 */
	_provides_sub_node(start_node)
		.for_each_sub_node("service",
		                   [&] (Xml_node node) { _add_service(node); });

	/*
	 * Construct inline config ROM service if "config" node is present.
	 */
	if (start_node.has_sub_node("config"))
		_config_rom_service.construct(*this);
}


Init::Child::~Child()
{
	_child_services.for_each([&] (Routed_service &service) {
		if (service.has_id_space(_session_requester.id_space()))
			destroy(_alloc, &service); });
}
