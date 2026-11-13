// Click-to-play: the YouTube player (youtube-nocookie.com) loads only when the visitor asks for
// it. Without JavaScript, or with a modified click, the link opens the video on YouTube.
document.addEventListener("click", function (e) {
  var a = e.target.closest && e.target.closest("a.video-link");
  if (!a || e.button !== 0 || e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
  e.preventDefault();
  var f = document.createElement("iframe");
  f.src = a.getAttribute("data-embed");
  f.title = "Woodland X11 demo (YouTube)";
  f.allow = "autoplay; encrypted-media; picture-in-picture; fullscreen";
  f.referrerPolicy = "strict-origin-when-cross-origin";
  a.replaceWith(f);
  f.focus();
});
