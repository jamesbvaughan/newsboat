#include "helpformaction.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "config.h"
#include "fmtstrformatter.h"
#include "keymap.h"
#include "listformatter.h"
#include "strprintf.h"
#include "utils.h"
#include "view.h"

namespace newsboat {

HelpFormAction::HelpFormAction(View* vv,
	std::string formstr,
	ConfigContainer* cfg,
	const std::string& ctx)
	: FormAction(vv, formstr, cfg)
	, quit(false)
	, apply_search(false)
	, context(ctx)
	, textview("helptext", FormAction::f)
{
}

HelpFormAction::~HelpFormAction() {}

bool HelpFormAction::process_operation(Operation op,
	const std::vector<std::string>& /* args */,
	BindingType /*bindingType*/)
{
	bool hardquit = false;
	switch (op) {
	case OP_QUIT:
		quit = true;
		break;
	case OP_HARDQUIT:
		hardquit = true;
		break;
	case OP_SEARCH: {
		std::vector<QnaPair> qna;
		qna.push_back(QnaPair(_("Search for: "), ""));
		this->start_qna(qna, OP_INT_START_SEARCH, &searchhistory);
	}
	break;
	case OP_CLEARFILTER:
		apply_search = false;
		do_redraw = true;
		break;
	default:
		if (handle_textview_operations(textview, op)) {
			break;
		}
		break;
	}
	if (hardquit) {
		while (v->formaction_stack_size() > 0) {
			v->pop_current_formaction();
		}
	} else if (quit) {
		v->pop_current_formaction();
	}
	return true;
}

void HelpFormAction::prepare()
{
	if (do_redraw) {
		recalculate_widget_dimensions();

		const unsigned int width = textview.get_width();

		FmtStrFormatter fmt;
		fmt.register_fmt('N', PROGRAM_NAME);
		fmt.register_fmt('V', utils::program_version());
		set_title(fmt.do_format(cfg->get_configvalue("help-title-format"), width));

		const auto descs = v->get_keymap()->get_keymap_descriptions(context);

		std::vector<std::string> colors = utils::tokenize(
				cfg->get_configvalue("search-highlight-colors"), " ");
		set_value("highlight", make_colorstring(colors));
		ListFormatter listfmt;

		std::vector<KeyMapDesc> syskey_descriptions;
		std::vector<KeyMapDesc> unbound_descriptions;
		std::vector<KeyMapDesc> bound_descriptions;

		for (const auto& desc : descs) {
			if (desc.flags & KM_SYSKEYS) {
				syskey_descriptions.push_back(desc);
			} else if (desc.key.empty()) {
				unbound_descriptions.push_back(desc);
			} else {
				bound_descriptions.push_back(desc);
			}
		}

		const auto should_be_visible = [&](const KeyMapDesc& desc) {
			return !apply_search
				|| strcasestr(desc.key.c_str(), searchphrase.c_str()) != nullptr
				|| strcasestr(desc.cmd.c_str(), searchphrase.c_str()) != nullptr
				|| strcasestr(desc.desc.c_str(), searchphrase.c_str()) != nullptr;
		};

		std::string highlighted_searchphrase = strprintf::fmt("<hl>%s</>", searchphrase);
		const auto apply_highlights = [&](const std::string& line) {
			if (apply_search && searchphrase.length() > 0) {
				return utils::replace_all(line, searchphrase, highlighted_searchphrase);
			}
			return line;
		};

		for (const auto& desc : bound_descriptions) {
			if (should_be_visible(desc)) {
				auto line = strprintf::fmt("%-15s %-23s %s", desc.key, desc.cmd, desc.desc);
				line = utils::quote_for_stfl(line);
				line = apply_highlights(line);
				listfmt.add_line(line);
			}
		}

		if (!syskey_descriptions.empty()) {
			listfmt.add_line("");
			listfmt.add_line(_("Generic bindings:"));
			listfmt.add_line("");

			for (const auto& desc : syskey_descriptions) {
				if (should_be_visible(desc)) {
					auto line = strprintf::fmt("%-15s %-23s %s", desc.key, desc.cmd, desc.desc);
					line = utils::quote_for_stfl(line);
					line = apply_highlights(line);
					listfmt.add_line(line);
				}
			}
		}

		if (!unbound_descriptions.empty()) {
			listfmt.add_line("");
			listfmt.add_line(_("Unbound functions:"));
			listfmt.add_line("");

			for (const auto& desc : unbound_descriptions) {
				if (should_be_visible(desc)) {
					std::string line = strprintf::fmt("%-39s %s", desc.cmd, desc.desc);
					line = utils::quote_for_stfl(line);
					line = apply_highlights(line);
					listfmt.add_line(line);
				}
			}
		}

		const auto macros = v->get_keymap()->get_macro_descriptions();
		if (!macros.empty()) {
			listfmt.add_line("");
			listfmt.add_line(_("Macros:"));
			listfmt.add_line("");

			for (const auto& macro : macros) {
				const std::string key = macro.first;
				const std::string description = macro.second.description;

				// "macro-prefix" is not translated because it refers to an operation name
				std::string line = strprintf::fmt("<macro-prefix>%s  %s", key, description);
				line = utils::quote_for_stfl(line);
				line = apply_highlights(line);
				listfmt.add_line(line);
			}
		}

		textview.stfl_replace_lines(listfmt.get_lines_count(), listfmt.format_list());

		do_redraw = false;
	}
	quit = false;
}

void HelpFormAction::init()
{
	set_keymap_hints();
}

const std::vector<KeyMapHintEntry>& HelpFormAction::get_keymap_hint() const
{
	static const std::vector<KeyMapHintEntry> hints = {{OP_QUIT, _("Quit")},
		{OP_SEARCH, _("Search")},
		{OP_CLEARFILTER, _("Clear")}
	};
	return hints;
}

void HelpFormAction::finished_qna(Operation op)
{
	v->inside_qna(false);
	switch (op) {
	case OP_INT_START_SEARCH:
		searchphrase = qna_responses[0];
		apply_search = true;
		do_redraw = true;
		break;
	default:
		FormAction::finished_qna(op);
		break;
	}
}

std::string HelpFormAction::title()
{
	return _("Help");
}

std::string HelpFormAction::make_colorstring(
	const std::vector<std::string>& colors)
{
	std::string result;
	if (colors.size() > 0) {
		if (colors[0] != "default") {
			result.append("fg=");
			result.append(colors[0]);
		}
		if (colors.size() > 1) {
			if (colors[1] != "default") {
				if (result.length() > 0) {
					result.append(",");
				}
				result.append("bg=");
				result.append(colors[1]);
			}
		}
		for (unsigned int i = 2; i < colors.size(); i++) {
			if (result.length() > 0) {
				result.append(",");
			}
			result.append("attr=");
			result.append(colors[i]);
		}
	}
	return result;
}

} // namespace newsboat
