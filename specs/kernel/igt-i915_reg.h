<!DOCTYPE html>
<html class="html-devise-layout gl-system" lang="en">
<head prefix="og: http://ogp.me/ns#">
<meta charset="utf-8">
<meta content="IE=edge" http-equiv="X-UA-Compatible">
<meta content="width=device-width, initial-scale=1" name="viewport">
<title>Sign in · GitLab</title>
<link as="font" crossorigin="" href="/assets/gitlab-sans/GitLabSans-9892dc17af892e03de41625c0ee325117a3b8ee4ba6005f3a3eac68510030aed.woff2" rel="preload">
<link as="font" crossorigin="" href="/assets/gitlab-sans/GitLabSans-Italic-f96f17332d67b21ada2dfba5f0c0e1d5801eab99330472057bf18edd93d4ccf7.woff2" rel="preload">
<link as="font" crossorigin="" href="/assets/gitlab-mono/GitLabMono-29c2152dac8739499dd0fe5cd37a486ebcc7d4798c9b6d3aeab65b3172375b05.woff2" rel="preload">
<link as="font" crossorigin="" href="/assets/gitlab-mono/GitLabMono-Italic-af36701a2188df32a9dcea12e0424c380019698d4f76da9ad8ea2fd59432cf83.woff2" rel="preload">

<script>
//<![CDATA[
window.gon={};gon.features={"twoStepSignIn":false};
//]]>
</script>

<script>
//<![CDATA[
const root = document.documentElement;
if (window.matchMedia('(prefers-color-scheme: dark)').matches) {
  root.classList.add('gl-dark');
}

window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', (e) => {
  if (e.matches) {
    root.classList.add('gl-dark');
  } else {
    root.classList.remove('gl-dark');
  }
});

//]]>
</script>




<meta content="light dark" name="color-scheme">
<link rel="stylesheet" href="/assets/application-2ded8e982afdeb378755a997c85e22b308944a372c54bb21c8ef16a996e326a7.css" media="(prefers-color-scheme: light)" />
<link rel="stylesheet" href="/assets/application_dark-f00cd8f62f167f2e27076533937ff07e4eff630068ad28b7d07bdcdb96f805dc.css" media="(prefers-color-scheme: dark)" />
<link rel="stylesheet" href="/assets/page_bundles/login-7240ec00cf3969b710fe5e8959f8ef8eeff66d635ded28839b1b4256ae8d96a3.css" /><link rel="stylesheet" href="/assets/page_bundles/commit_description-9e7efe20f0cef17d0606edabfad0418e9eb224aaeaa2dae32c817060fa60abcc.css" /><link rel="stylesheet" href="/assets/page_bundles/work_items-ffaabf548efb8bd9bee71a5704a0e41e4db5af0d502028c053a88ea3ebc702f9.css" /><link rel="stylesheet" href="/assets/page_bundles/notes_shared-1a99012f97ba760b6bc57563b5b2863243958d14d14a25fee12843f0268c2731.css" />
<link rel="stylesheet" href="/assets/tailwind_cqs-b1b4e9e5c6238ff26341ede69f7ea5b2797112801aa5e62fbfb8bf85d5d65260.css" />


<link rel="stylesheet" href="/assets/fonts-deb7ad1d55ca77c0172d8538d53442af63604ff490c74acc2859db295c125bdb.css" />
<link rel="stylesheet" href="/assets/highlight/themes/white-7ea69b41b9a1d48ec0f546f19e9c0bef9cedaa3cd15f06ee433460ce1b2461b0.css" media="(prefers-color-scheme: light)" />
<link rel="stylesheet" href="/assets/highlight/themes/dark-9735b2efe7ac5369a897452ddf41f2df0e0136260be9e29a3856f0d064f735e9.css" media="(prefers-color-scheme: dark)" />

<script src="/assets/webpack/runtime.60f2662c.bundle.js" defer="defer"></script>
<script src="/assets/webpack/main.0fac458b.chunk.js" defer="defer"></script>
<script src="/assets/webpack/tracker.d838be2f.chunk.js" defer="defer"></script>
<script>
//<![CDATA[
window.snowplowOptions = {"namespace":"gl","hostname":"gitlab.freedesktop.org:443","postPath":"/-/collect_events","forceSecureTracker":true,"appId":"gitlab_sm"};
gl = window.gl || {};
gl.snowplowStandardContext = {"schema":"iglu:com.gitlab/gitlab_standard/jsonschema/1-1-8","data":{"environment":"self-managed","source":"gitlab-rails","correlation_id":"01KY0Z0N35N2HQKFDRQY6V6N4J","extra":{},"user_id":null,"global_user_id":null,"user_type":null,"is_gitlab_team_member":null,"namespace_id":null,"ultimate_parent_namespace_id":null,"project_id":null,"feature_enabled_by_namespace_ids":null,"realm":"self-managed","deployment_type":"self-managed","context_generated_at":"2026-07-20T23:50:39.007Z","organization_id":1}};
gl.snowplowPseudonymizedPageUrl = "https://gitlab.freedesktop.org/users/sign_in";
gl.maskedDefaultReferrerUrl = null;
gl.ga4MeasurementId = 'G-ENFH3X7M5Y';
gl.duoEvents = [];
gl.onlySendDuoEvents = false;


//]]>
</script>



<script src="/assets/webpack/commons-pages.admin.application_settings.service_accounts-pages.explore.analytics_dashboards-pages.e-306889d5.c005c5a9.chunk.js" defer="defer"></script>
<script src="/assets/webpack/commons-pages.search.show-super_sidebar.67d9a544.chunk.js" defer="defer"></script>
<script src="/assets/webpack/super_sidebar.792962d8.chunk.js" defer="defer"></script>
<script src="/assets/webpack/commons-pages.admin.sessions-pages.ldap.omniauth_callbacks-pages.omniauth_callbacks-pages.sessions-p-ea3be603.ac74f4f7.chunk.js" defer="defer"></script>
<script src="/assets/webpack/commons-pages.registrations.new-pages.sessions.new.bad8999e.chunk.js" defer="defer"></script>
<script src="/assets/webpack/pages.sessions.new.ddb39580.chunk.js" defer="defer"></script>



<meta content="object" property="og:type">
<meta content="GitLab" property="og:site_name">
<meta content="Sign in · GitLab" property="og:title">
<meta content="freedesktop.org GitLab login" property="og:description">
<meta content="https://gitlab.freedesktop.org/assets/twitter_card-570ddb06edf56a2312253c5872489847a0f385112ddbcd71ccfa1570febab5d2.jpg" property="og:image">
<meta content="64" property="og:image:width">
<meta content="64" property="og:image:height">
<meta content="https://gitlab.freedesktop.org/users/sign_in" property="og:url">
<meta content="summary" property="twitter:card">
<meta content="Sign in · GitLab" property="twitter:title">
<meta content="freedesktop.org GitLab login" property="twitter:description">
<meta content="https://gitlab.freedesktop.org/assets/twitter_card-570ddb06edf56a2312253c5872489847a0f385112ddbcd71ccfa1570febab5d2.jpg" property="twitter:image">

<meta name="csrf-param" content="authenticity_token" />
<meta name="csrf-token" content="_gYXKPbunyPAWUFw1msFhATvYNVi4DtUNrCAIuv-UJ_sHcoddDunu5b3iN1vCosbIuwuMXVMYPttAvdRjyGUhw" />
<meta name="csp-nonce" />
<meta name="action-cable-url" content="/-/cable" />
<link href="/-/manifest.json" rel="manifest">
<link rel="icon" type="image/png" href="/uploads/-/system/appearance/favicon/1/fdo-favicon.ico" id="favicon" data-original-href="/uploads/-/system/appearance/favicon/1/fdo-favicon.ico" />
<link rel="apple-touch-icon" type="image/x-icon" href="/assets/apple-touch-icon-b049d4bc0dd9626f31db825d61880737befc7835982586d015bded10b4435460.png" />
<link href="/search/opensearch.xml" rel="search" title="Search GitLab" type="application/opensearchdescription+xml">




<meta content="freedesktop.org GitLab login" name="description">
<meta content="#F1F0F6" media="(prefers-color-scheme: light)" name="theme-color">
<meta content="#232128" media="(prefers-color-scheme: dark)" name="theme-color">
</head>

<body class="gl-h-full login-page gl-browser-generic gl-platform-other" data-page="sessions:new" data-testid="login-page">

<script>
//<![CDATA[
gl = window.gl || {};
gl.client = {"isGeneric":true,"isOther":true};


//]]>
</script>
<div class="gl-broadcast-message banner js-broadcast-notification-10 light-red" data-broadcast-banner data-testid="banner-broadcast-message" role="alert">
<div class="gl-broadcast-message-content">
<div class="gl-broadcast-message-icon">
<svg class="s16" data-testid="bullhorn-icon"><use href="/assets/icons-744c96d42bcc6345fdb1c870432a980c5f983ed177df344a4a5f94b625ac4ce8.svg#bullhorn"></use></svg>
</div>
<div class="gl-broadcast-message-text">
<h2 class="gl-sr-only">Admin message</h2>
<p>Due to an influx of spam, we have had to impose restrictions on new accounts. Please see <a href="https://gitlab.freedesktop.org/freedesktop/freedesktop/-/wikis/home">this wiki page</a> for instructions on how to get full permissions. Sorry for the inconvenience.</p>
</div>
</div>
<button class="gl-button btn btn-icon btn-sm btn-default btn-default-tertiary gl-broadcast-message-dismiss js-dismiss-current-broadcast-notification" aria-label="Close" data-id="10" data-expire-date="2030-12-01T21:21:05Z" data-cookie-key="hide_broadcast_message_10" type="button"><svg class="s16 gl-icon gl-button-icon " data-testid="close-icon"><use href="/assets/icons-744c96d42bcc6345fdb1c870432a980c5f983ed177df344a4a5f94b625ac4ce8.svg#close"></use></svg>

</button>
</div>






<div class="gl-h-full gl-flex gl-flex-wrap">
<div class="container gl-self-center">
<main class="content">
<div class="flash-container flash-container-page sticky" data-testid="flash-container">
<div id="js-global-alerts"></div>
</div>

<div class="row gl-mt-5 gl-gap-y-6">
<div class="col-md order-12">
<div class="col-sm-12">
<h1 class="gl-mb-5 gl-text-size-h2 gl-hidden md:gl-block">
freedesktop.org GitLab login
</h1>
<div class="md" id="js-custom-sign-in-description"><p data-sourcepos="1:1-1:271" dir="auto">freedesktop.org is a community of open-source projects dedicated to advancing the open-source desktop ecosystem. If you would like to contribute code, bug reports, or ideas to our existing member projects, you can register or log in here and contribute to these projects.</p>&#x000A;<p data-sourcepos="3:1-3:487" dir="auto"><strong data-sourcepos="3:1-3:487">When creating a new account, you will be sent an email to confirm and validate your account. Please check your inbox (including spam) to ensure that you receive this message and follow the link to confirm your account. Once this is done, you will be able to report issues in existing projects. To fork projects, you will need to <a data-sourcepos="3:332-3:485" href="https://gitlab.freedesktop.org/freedesktop/freedesktop/-/wikis/home#how-can-i-contribute-to-an-existing-project-or-create-a-new-one">request permissions</a></strong></p>&#x000A;<p data-sourcepos="5:1-5:295" dir="auto"><strong data-sourcepos="5:1-5:295">When creating a new account, you will be sent an email to confirm and validate your account. Please check your inbox (including spam) to ensure that you receive this message and follow the link to confirm your account. Once this is done, your account will become active within 10-15 minutes.</strong></p>&#x000A;<p data-sourcepos="7:1-7:292" dir="auto">Please conduct yourself in a polite and respectful way when using these services, per our <a data-sourcepos="7:91-7:155" href="https://www.freedesktop.org/wiki/CodeOfConduct" rel="nofollow noreferrer noopener" target="_blank">Code of Conduct</a>. The information we collect and how we use it is also described in our <a data-sourcepos="7:228-7:291" href="https://www.freedesktop.org/wiki/PrivacyPolicy" rel="nofollow noreferrer noopener" target="_blank">Privacy Policy</a>.</p></div>
</div>
</div>
<div class="col-md order-md-12">
<div class="col-sm-12 bar">
<div class="gl-text-center gl-mb-5">
<img alt="freedesktop.org GitLab login" class="gl-invisible gl-h-10 js-portrait-logo-detection lazy" data-src="/assets/logo-911de323fa0def29aaf817fca33916653fc92f3ff31647ac41d2c39bbe243edb.svg" src="data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==" />
<h1 class="gl-mb-3 gl-text-size-h2 md:gl-hidden">
freedesktop.org GitLab login
</h1>
</div>

<div class="js-non-oauth-login">
<div data-app-data="{&quot;sign_in_path&quot;:&quot;/users/sign_in&quot;,&quot;users_sign_in_path_path&quot;:&quot;/users/sign_in_path&quot;,&quot;passkeys_sign_in_path&quot;:&quot;/users/passkeys/sign_in&quot;,&quot;is_unconfirmed_email&quot;:false,&quot;new_user_confirmation_path&quot;:&quot;/users/confirmation/new&quot;,&quot;new_password_path&quot;:&quot;/users/password/new&quot;,&quot;show_captcha&quot;:false,&quot;is_remember_me_enabled&quot;:true}" id="js-sign-in-form">
<div class="gl-spinner-container gl-my-5" role="status"><span aria-hidden class="gl-spinner gl-spinner-md gl-spinner-dark !gl-align-text-bottom"></span><span class="gl-sr-only !gl-absolute">Loading</span>
</div>
<form action="/users/sign_in" accept-charset="UTF-8" method="post"><input type="hidden" name="authenticity_token" value="xnRFpEA7gztqcsbzX8X2rUuJkGjbkq-KtY3o124Qc_LUb5iRwu67ozzcD17mpHgybYrejMw-9CXuP5-kCs-36g" autocomplete="off" /><input data-js-name="login" autocomplete="off" type="hidden" name="user[login]" id="user_login" />
<input data-js-name="password" autocomplete="off" type="hidden" name="user[password]" id="user_password" />
<input data-js-name="rememberMe" autocomplete="off" type="hidden" name="user[remember_me]" id="user_remember_me" />
</form></div>

</div>
<div class="gl-mt-3">
By signing in you accept the <a href='/-/users/terms' target='_blank' rel='noreferrer noopener'>Terms of Use and acknowledge the Privacy Statement and Cookie Policy</a>.
</div>
<div class="gl-mt-3 gl-text-center">
Don&#39;t have an account yet?
<a data-testid="register-link" data-track-action="click_register_from_sign_in_page" href="/users/sign_up">Register now</a>
</div>
<div class="gl-flex gl-items-center gl-gap-5" data-testid="divider">
<hr class="gl-grow gl-border-default">
or sign in with
<hr class="gl-grow gl-border-default">
</div>

<div class="gl-mt-5 gl-text-center gl-flex gl-flex-col gl-gap-3 js-oauth-login">
<form class="js-omniauth-form" method="post" action="/users/auth/google_oauth2"><button class="gl-button btn btn-block btn-md btn-default " type="submit"><span class="gl-button-text">
<img alt="Google" title="Sign in with Google" class="gl-button-icon lazy" data-src="/assets/auth_buttons/google_64-9ab7462cd2115e11f80171018d8c39bd493fc375e83202fbb6d37a487ad01908.png" src="data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==" />
<span class="gl-button-text">
Google
</span>

</span>

</button><input type="hidden" name="authenticity_token" value="11UdNqAwC3o0bp9hVdE81yc7n0qxUGSyuv1V2rYEhHXFTsADIuUz4mLAVszssLJIATjRrqb8Px3hTyKp0ttAbQ" autocomplete="off" /></form>
<form class="js-omniauth-form" method="post" action="/users/auth/github"><button class="gl-button btn btn-block btn-md btn-default " data-testid="github-login-button" type="submit"><span class="gl-button-text">
<img alt="GitHub" title="Sign in with GitHub" class="gl-button-icon lazy" data-src="/assets/auth_buttons/github_64-84041cd0ea392220da96f0fb9b9473c08485c4924b98c776be1bd33b0daab8c0.png" src="data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==" />
<span class="gl-button-text">
GitHub
</span>

</span>

</button><input type="hidden" name="authenticity_token" value="SqHktm9ymKxO_Zb8gjbCp9SSysk1O0D7WKU4kYMP4ctYujmD7aegNBhTX1E7V0w48pGELSKXG1QDF0_i59Al0w" autocomplete="off" /></form>
<form class="js-omniauth-form" method="post" action="/users/auth/gitlab"><button class="gl-button btn btn-block btn-md btn-default " data-testid="gitlab-oauth-login-button" type="submit"><span class="gl-button-text">
<img alt="GitLab" title="Sign in with GitLab" class="gl-button-icon lazy" data-src="/assets/auth_buttons/gitlab_64-2808a5fce900f4cb87b4dce9f9907bf68ac00a39086aa3a125e3250652f9967f.png" src="data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==" />
<span class="gl-button-text">
GitLab
</span>

</span>

</button><input type="hidden" name="authenticity_token" value="C1wZ_0JLVHsrRDsF_cS_0mnPtzb_X80I99ZroUAD7pMZR8TKwJ5s433q8qhEpTFNT8z50ujzlqesZBzSJNwqiw" autocomplete="off" /></form>
<div class="gl-form-checkbox custom-control custom-checkbox">
<input type="checkbox" name="js-remember-me-omniauth" id="js-remember-me-omniauth" class="custom-control-input" />
<label class="custom-control-label" for="js-remember-me-omniauth"><span>Remember me
</span></label>
</div>
</div>


</div>
</div>
</div>
</main>
</div>
<div class="footer-container gl-w-full gl-self-end">
<hr class="gl-m-0">
<div class="container gl-py-5 gl-flex gl-justify-between gl-items-start">
<div class="gl-hidden md:gl-flex gl-gap-5 gl-flex-wrap">
<a href="/explore">Explore</a>
<a href="/help">Help</a>
<a href="https://about.gitlab.com">About GitLab</a>
<a target="_blank" class="text-nowrap" rel="noopener noreferrer" href="https://forum.gitlab.com">GitLab community forum</a>
</div>
<div class="js-language-switcher" data-locales="[{&quot;value&quot;:&quot;en&quot;,&quot;percentage&quot;:100,&quot;text&quot;:&quot;English&quot;},{&quot;value&quot;:&quot;ja&quot;,&quot;percentage&quot;:99,&quot;text&quot;:&quot;日本語&quot;},{&quot;value&quot;:&quot;ga_IE&quot;,&quot;percentage&quot;:99,&quot;text&quot;:&quot;Gaeilge&quot;},{&quot;value&quot;:&quot;pt_BR&quot;,&quot;percentage&quot;:96,&quot;text&quot;:&quot;português (Brasil)&quot;},{&quot;value&quot;:&quot;it&quot;,&quot;percentage&quot;:96,&quot;text&quot;:&quot;italiano&quot;},{&quot;value&quot;:&quot;fr&quot;,&quot;percentage&quot;:96,&quot;text&quot;:&quot;français&quot;},{&quot;value&quot;:&quot;ko&quot;,&quot;percentage&quot;:95,&quot;text&quot;:&quot;한국어&quot;},{&quot;value&quot;:&quot;es&quot;,&quot;percentage&quot;:95,&quot;text&quot;:&quot;español&quot;},{&quot;value&quot;:&quot;de&quot;,&quot;percentage&quot;:91,&quot;text&quot;:&quot;Deutsch&quot;}]"></div>

</div>
</div>


</div>
</body>
</html>
