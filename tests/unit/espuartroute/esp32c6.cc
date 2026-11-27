// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "routes.h"
#include "uart_routes.h"

TEST(EspUartRoute, Esp32c6EachUartPostsItsOwnLine)
{
    espuartroute::each_posts_its_own(kickos::esp32c6::UART_ROUTES);
}

TEST(EspUartRoute, Esp32c6AllAssertingPostAllButTheMasked)
{
    espuartroute::all_post_all(kickos::esp32c6::UART_ROUTES);
}

TEST(EspUartRoute, Esp32c6NoUartAssertedPostsNothing)
{
    espuartroute::none_posts_none(kickos::esp32c6::UART_ROUTES);
}
