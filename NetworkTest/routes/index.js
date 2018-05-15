const router = require('koa-router')();

router.get('/', async (ctx, next) => {
    await ctx.redirect('homepage');
});

router.get('/homepage', async (ctx, next) => {
    await ctx.render('home-page')
});

router.get('/chartpage', async(ctx, next) => {
    await ctx.render('chart-page')
});

module.exports = router;
