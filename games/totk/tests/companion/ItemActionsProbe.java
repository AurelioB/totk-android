import com.aure.totk.companion.ItemActions;

/** Host checks for companion request identity, duplicate-tap dropping and result lifetime. */
public final class ItemActionsProbe {
    private static void check(boolean condition, String what) {
        if (!condition) throw new AssertionError(what);
    }
    private static ItemActions.Request eat() { return new ItemActions.Request("eat", "food", 3, "Item_Cook_A_01", 0, 41); }

    public static void main(String[] args) {
        ItemActions actions = new ItemActions();
        ItemActions.Request first = eat();
        check(actions.tap(first), "first tap queued");
        check(!actions.tap(eat()), "duplicate tap while queued is dropped");
        check(!actions.tap(new ItemActions.Request("hold", "materials", 1, "Item_Fruit_A", 0, 41)), "other action while queued is dropped");
        check(actions.take() == first, "the queued request keeps its identity");
        check(actions.take() == null, "a request is handed out once");
        actions.submitted(7, 1000);
        check(actions.pending() == 7, "accepted request is pending");
        check(!actions.tap(eat()), "duplicate tap while pending is dropped");
        actions.answered(6, "done", "", 1100);
        check(actions.pending() == 7, "answers for another seq are ignored");
        actions.answered(7, "pending", "", 1200);
        actions.answered(7, null, "", 1300);
        check(actions.pending() == 7, "pending and missing answers keep waiting");
        actions.answered(7, "done", "", 1400);
        ItemActions.State done = actions.state();
        check(done.phase == ItemActions.Phase.DONE && done.request == first, "done keeps the request");
        check(done.request.sameItem("food", 3, "Item_Cook_A_01") && !done.request.sameItem("food", 3, "Item_Cook_B_01"),
                "identity is category, slot and actor");
        check(actions.take() == null, "a settled request is never resubmitted");

        actions.observed("after-eat", 1500);
        actions.observed("after-eat-2", 2000);
        check(actions.state().phase == ItemActions.Phase.DONE, "changes inside the minimum display time are absorbed");
        actions.observed("baseline", 2900);
        actions.observed("baseline", 3500);
        check(actions.state().phase == ItemActions.Phase.DONE, "an unchanged snapshot keeps the result");
        actions.observed("next", 3600);
        check(actions.state().phase == ItemActions.Phase.IDLE && actions.state().request == null, "the next change clears the result");

        check(actions.tap(eat()), "tap after a result is a new request");
        actions.take();
        actions.submitted(-1, 4000);
        check(actions.state().phase == ItemActions.Phase.REFUSED && "busy".equals(actions.state().reason), "busy is a refusal");
        check(actions.tap(eat()), "tap after busy refusal is accepted");
        actions.take();
        actions.submitted(-3, 4100);
        check(actions.state().phase == ItemActions.Phase.FAILED && "unsupported".equals(actions.state().reason), "unsupported fails");

        check(actions.tap(eat()), "new tap");
        actions.take();
        actions.submitted(9, 5000);
        actions.answered(9, "refused", "fullHearts", 5100);
        check(actions.state().phase == ItemActions.Phase.REFUSED && "fullHearts".equals(actions.state().reason), "refusal reason kept");

        check(actions.tap(eat()), "new tap");
        actions.take();
        actions.submitted(10, 6000);
        actions.answered(10, "pending", "", 6000 + ItemActions.ANSWER_TIMEOUT_MS);
        check(actions.state().phase == ItemActions.Phase.FAILED && "noAnswer".equals(actions.state().reason),
                "a lost answer fails once, without retry");
        check(actions.take() == null, "no automatic retry");

        int before = actions.version();
        check(actions.tap(eat()) && actions.version() != before, "state changes bump the redraw version");
        check(!actions.tap(null), "null request is ignored");
        System.out.println("pass: companion item actions");
    }
}
