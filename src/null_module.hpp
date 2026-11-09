// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Placeholder modules: a tone module that plays nothing and effects that pass the dry bus
// through. Used while an engine model is being built.

#include "module.hpp"

namespace hvyxg {

class NullToneModule final : public ToneModule {
public:
    explicit NullToneModule(std::size_t block) : block_(block) {}
    std::size_t block_size() const override { return block_; }
    void note_on(const midi::Part&, std::uint8_t, std::uint8_t) override {}
    void note_off(const midi::Part&, std::uint8_t) override {}
    void sustain_changed(const midi::Part&) override {}
    void sostenuto_changed(const midi::Part&) override {}
    void release_all(const midi::Part&) override {}
    void kill_all(const midi::Part&) override {}
    void render(Buses&) override {}
    bool active() const override { return false; }
    std::size_t voice_count() const override { return 0; }

private:
    std::size_t block_;
};

class DryEffects final : public SystemEffects {
public:
    void process(Buses&, std::size_t) override {}
    bool active() const override { return false; }
};

} // namespace hvyxg
